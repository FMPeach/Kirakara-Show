#include "native_stage_program_clock.h"

#include <algorithm>
#include <cmath>

namespace {

double finite_non_negative(double value) noexcept {
    return std::isfinite(value) && value > 0.0 ? value : 0.0;
}

double buffer_runway(
        const NativeStageVideoBufferSnapshot& buffer,
        double position_seconds) noexcept {
    return std::max(0.0,
        finite_non_negative(buffer.end_seconds)
            - finite_non_negative(position_seconds));
}

bool buffer_contains(
        const NativeStageVideoBufferSnapshot& buffer,
        double position_seconds,
        double tolerance_seconds) noexcept {
    if (buffer.frame_count == 0) return false;
    const auto position = finite_non_negative(position_seconds);
    const auto tolerance = finite_non_negative(tolerance_seconds);
    return finite_non_negative(buffer.start_seconds) <= position + tolerance
        && finite_non_negative(buffer.end_seconds) + tolerance >= position;
}

bool buffer_covers_declared_end(
        const NativeStageVideoBufferSnapshot& buffer,
        double tolerance_seconds) noexcept {
    const auto duration = finite_non_negative(buffer.source_duration_seconds);
    return buffer.frame_count > 0 && duration > 0.0
        && finite_non_negative(buffer.end_seconds)
            + finite_non_negative(tolerance_seconds) >= duration;
}

bool holds_video_tail_for_audio(
        const NativeStageProgramClockInput& input) noexcept {
    return input.end_on_audio_eof
        && input.has_audio_clock
        && input.has_video
        && input.video_buffer.frame_count > 0
        && input.video_buffer.availability
            == MfD3D11VideoAvailability::end_of_stream;
}

double effective_video_end(
        const NativeStageVideoBufferSnapshot& buffer) noexcept {
    const auto duration = finite_non_negative(buffer.source_duration_seconds);
    return duration > 0.0
        ? duration : finite_non_negative(buffer.end_seconds);
}

bool video_ready_to_resume(
        const NativeStageProgramClockInput& input,
        double position_seconds,
        const NativeStageProgramClockConfig& config,
        bool require_runway) noexcept {
    if (!input.has_video || holds_video_tail_for_audio(input)) return true;
    if (input.video_buffer.availability
            == MfD3D11VideoAvailability::fatal_error) {
        return false;
    }
    if (!buffer_contains(input.video_buffer, position_seconds,
            config.timestamp_tolerance_seconds)) {
        return false;
    }
    if (!require_runway
            || input.video_buffer.availability
                == MfD3D11VideoAvailability::end_of_stream
            || buffer_covers_declared_end(
                input.video_buffer, config.timestamp_tolerance_seconds)) {
        return true;
    }
    const auto runway = buffer_runway(
        input.video_buffer, position_seconds);
    return runway >= finite_non_negative(config.recovery_runway_seconds)
        || (runway > finite_non_negative(
                config.timestamp_tolerance_seconds)
            && input.video_buffer.frame_count
                >= std::max<std::size_t>(
                    1, config.minimum_recovery_frames));
}

bool video_is_truly_unavailable(
        const NativeStageProgramClockInput& input,
        double position_seconds,
        const NativeStageProgramClockConfig& config) noexcept {
    if (!input.has_video || holds_video_tail_for_audio(input)) return false;
    if (buffer_contains(input.video_buffer, position_seconds,
            config.timestamp_tolerance_seconds)) {
        return false;
    }
    if (input.video_buffer.availability
            == MfD3D11VideoAvailability::end_of_stream) {
        return input.video_buffer.frame_count == 0;
    }
    return input.video_buffer.read_in_flight
        || input.video_buffer.availability
            == MfD3D11VideoAvailability::opening
        || input.video_buffer.availability
            == MfD3D11VideoAvailability::ready
        || input.video_buffer.availability
            == MfD3D11VideoAvailability::retryable_starvation;
}

}  // namespace

NativeStageProgramClock::NativeStageProgramClock(
        NativeStageProgramClockConfig config) noexcept
    : config_(config) {}

void NativeStageProgramClock::reset(
        double position_seconds, double monotonic_seconds) {
    std::lock_guard lock(mutex_);
    position_seconds_ = finite_non_negative(position_seconds);
    state_ = NativeStageProgramClockState::idle;
    anchor(position_seconds_, monotonic_seconds);
}

void NativeStageProgramClock::discontinuity(
        double position_seconds, double monotonic_seconds) {
    std::lock_guard lock(mutex_);
    position_seconds_ = finite_non_negative(position_seconds);
    state_ = NativeStageProgramClockState::idle;
    anchor(position_seconds_, monotonic_seconds);
}

void NativeStageProgramClock::pause_at(
        double position_seconds, double monotonic_seconds) {
    std::lock_guard lock(mutex_);
    position_seconds_ = finite_non_negative(position_seconds);
    if (state_ != NativeStageProgramClockState::idle
            && state_ != NativeStageProgramClockState::ended
            && state_ != NativeStageProgramClockState::fatal_error) {
        state_ = NativeStageProgramClockState::paused;
    }
    anchor(position_seconds_, monotonic_seconds);
}

NativeStageProgramClockUpdate NativeStageProgramClock::update(
        const NativeStageProgramClockInput& input) {
    std::lock_guard lock(mutex_);
    const auto now = std::isfinite(input.monotonic_seconds)
        ? input.monotonic_seconds : anchor_monotonic_seconds_;
    const auto audio_position = finite_non_negative(
        input.audio_position_seconds);
    const auto previous_state = state_;
    const bool has_audio_stream =
        input.has_audio_stream || input.has_audio_clock;

    if (!input.playback_requested) {
        if (state_ == NativeStageProgramClockState::playing
                && input.has_audio_clock) {
            position_seconds_ = std::max(position_seconds_, audio_position);
        }
        if (state_ != NativeStageProgramClockState::idle
                && state_ != NativeStageProgramClockState::ended
                && state_ != NativeStageProgramClockState::fatal_error) {
            state_ = NativeStageProgramClockState::paused;
        }
        anchor(position_seconds_, now);
        auto update = result();
        update.request_audio_pause = has_audio_stream
            && previous_state != NativeStageProgramClockState::idle
            && previous_state != NativeStageProgramClockState::paused;
        return update;
    }

    if (input.has_video
            && input.video_buffer.availability
                == MfD3D11VideoAvailability::fatal_error) {
        state_ = NativeStageProgramClockState::fatal_error;
        anchor(position_seconds_, now);
        auto update = result();
        update.request_audio_pause = has_audio_stream
            && previous_state != NativeStageProgramClockState::fatal_error;
        return update;
    }

    if (state_ == NativeStageProgramClockState::ended
            || state_ == NativeStageProgramClockState::fatal_error) {
        return result();
    }

    if (input.end_on_audio_eof && input.has_audio_clock
            && input.audio_end_of_stream) {
        const auto audio_end = finite_non_negative(
            input.audio_duration_seconds);
        position_seconds_ = audio_end > 0.0
            ? audio_end : std::max(position_seconds_, audio_position);
        state_ = NativeStageProgramClockState::ended;
        anchor(position_seconds_, now);
        auto update = result();
        update.request_audio_pause = has_audio_stream;
        return update;
    }

    if (state_ == NativeStageProgramClockState::paused) {
        if (video_ready_to_resume(
                input, position_seconds_, config_, false)) {
            state_ = NativeStageProgramClockState::playing;
            anchor(position_seconds_, now);
            auto update = result();
            if (has_audio_stream) {
                update.request_audio_seek = true;
                update.request_audio_play = true;
                update.audio_seek_position_seconds = position_seconds_;
            }
            return update;
        }
        state_ = input.has_video
            ? NativeStageProgramClockState::priming
            : NativeStageProgramClockState::playing;
        anchor(position_seconds_, now);
    } else if (state_ == NativeStageProgramClockState::idle) {
        state_ = input.has_video
            ? NativeStageProgramClockState::priming
            : NativeStageProgramClockState::playing;
        anchor(position_seconds_, now);
        if (!input.has_video) {
            auto update = result();
            if (has_audio_stream) {
                update.request_audio_seek = true;
                update.request_audio_play = true;
                update.audio_seek_position_seconds = position_seconds_;
            }
            return update;
        }
    }

    if (state_ == NativeStageProgramClockState::priming
            || state_ == NativeStageProgramClockState::buffering) {
        if (!video_ready_to_resume(
                input, position_seconds_, config_, true)) {
            auto update = result();
            update.request_audio_pause = has_audio_stream
                && previous_state != NativeStageProgramClockState::priming
                && previous_state != NativeStageProgramClockState::buffering;
            return update;
        }
        state_ = NativeStageProgramClockState::playing;
        anchor(position_seconds_, now);
        auto update = result();
        if (has_audio_stream) {
            update.request_audio_seek = true;
            update.request_audio_play = true;
            update.audio_seek_position_seconds = position_seconds_;
        }
        return update;
    }

    double candidate = position_seconds_;
    if (input.has_audio_clock) {
        candidate = std::max(position_seconds_, audio_position);
    } else {
        candidate = anchor_position_seconds_
            + std::max(0.0, now - anchor_monotonic_seconds_);
    }

    if (input.has_video && !input.end_on_audio_eof) {
        const auto end = effective_video_end(input.video_buffer);
        if (end > 0.0
                && candidate + config_.timestamp_tolerance_seconds >= end) {
            position_seconds_ = end;
            state_ = NativeStageProgramClockState::ended;
            anchor(position_seconds_, now);
            auto update = result();
            update.request_audio_pause = has_audio_stream;
            return update;
        }
    }

    if (video_is_truly_unavailable(input, candidate, config_)) {
        state_ = NativeStageProgramClockState::buffering;
        anchor(position_seconds_, now);
        auto update = result();
        update.request_audio_pause = has_audio_stream;
        return update;
    }

    position_seconds_ = candidate;
    return result();
}

double NativeStageProgramClock::position_seconds() const {
    std::lock_guard lock(mutex_);
    return position_seconds_;
}

NativeStageProgramClockState NativeStageProgramClock::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

void NativeStageProgramClock::anchor(
        double position_seconds, double monotonic_seconds) noexcept {
    anchor_position_seconds_ = finite_non_negative(position_seconds);
    anchor_monotonic_seconds_ = std::isfinite(monotonic_seconds)
        ? monotonic_seconds : 0.0;
}

NativeStageProgramClockUpdate NativeStageProgramClock::result() const noexcept {
    NativeStageProgramClockUpdate update;
    update.position_seconds = position_seconds_;
    update.state = state_;
    return update;
}

NativeStageVideoBufferSnapshot native_stage_buffer_snapshot(
        const MfD3D11VideoBufferStatus& status) noexcept {
    NativeStageVideoBufferSnapshot snapshot;
    snapshot.frame_count = status.frame_count;
    snapshot.start_seconds = status.buffered_start_seconds;
    // A decoded future sample proves that the preceding picture is safe to
    // hold until that PTS. Treat such low-frame-rate/VFR gaps as buffered
    // presentation time rather than transport starvation. Cast deliberately
    // retains its stricter contiguous watermark.
    snapshot.end_seconds = std::max(
        status.buffered_end_seconds, status.queued_end_seconds);
    snapshot.source_duration_seconds = status.source_duration_seconds;
    snapshot.availability = status.availability;
    snapshot.read_in_flight = status.read_in_flight;
    return snapshot;
}

