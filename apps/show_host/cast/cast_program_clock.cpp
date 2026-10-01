#include "cast_program_clock.h"

#include <algorithm>
#include <cmath>

namespace {

double finite_non_negative(double value) noexcept {
    return std::isfinite(value) && value > 0.0 ? value : 0.0;
}

double effective_video_end(
        const CastVideoBufferSnapshot& buffer) noexcept {
    const auto duration = finite_non_negative(
        buffer.source_duration_seconds);
    return duration > 0.0
        ? duration : finite_non_negative(buffer.end_seconds);
}

}  // namespace

CastProgramClock::CastProgramClock(
        CastVideoBufferWatermarks watermarks) noexcept
    : watermarks_(watermarks) {
    watermarks_.low_seconds = finite_non_negative(watermarks_.low_seconds);
    watermarks_.high_seconds = std::max(
        watermarks_.low_seconds,
        finite_non_negative(watermarks_.high_seconds));
    watermarks_.timestamp_tolerance_seconds = finite_non_negative(
        watermarks_.timestamp_tolerance_seconds);
    watermarks_.minimum_start_frames = std::max<std::size_t>(
        1, watermarks_.minimum_start_frames);
}

void CastProgramClock::reset(
        double position_seconds, double monotonic_seconds) {
    std::lock_guard lock(mutex_);
    state_ = CastProgramClockState::idle;
    anchor(position_seconds, monotonic_seconds);
}

void CastProgramClock::discontinuity(
        double position_seconds, double monotonic_seconds) {
    std::lock_guard lock(mutex_);
    state_ = CastProgramClockState::idle;
    anchor(position_seconds, monotonic_seconds);
}

void CastProgramClock::pause_at(
        double position_seconds, double monotonic_seconds) {
    std::lock_guard lock(mutex_);
    if (state_ != CastProgramClockState::idle
            && state_ != CastProgramClockState::ended
            && state_ != CastProgramClockState::fatal_error) {
        state_ = CastProgramClockState::paused;
    }
    anchor(position_seconds, monotonic_seconds);
}

CastProgramClockUpdate CastProgramClock::update(
        const CastProgramClockInput& input) {
    std::lock_guard lock(mutex_);
    const auto now = std::isfinite(input.monotonic_seconds)
        ? input.monotonic_seconds : anchor_monotonic_seconds_;
    const auto audio_position = finite_non_negative(
        input.audio_position_seconds);
    const auto previous_state = state_;
    const bool has_audio_stream =
        input.has_audio_stream || input.has_audio_clock;
    const bool hold_video_tail_for_audio = input.end_on_audio_eof
        && input.has_audio_clock
        && input.has_video
        && input.video_buffer.frame_count > 0
        && input.video_buffer.availability
            == CastVideoBufferAvailability::end_of_stream;

    if (!input.playback_requested) {
        if (state_ == CastProgramClockState::playing
                && input.has_audio_clock) {
            position_seconds_ = std::max(position_seconds_, audio_position);
        }
        if (state_ != CastProgramClockState::idle
                && state_ != CastProgramClockState::ended
                && state_ != CastProgramClockState::fatal_error) {
            state_ = CastProgramClockState::paused;
        }
        anchor(position_seconds_, now);
        auto update = result();
        update.request_audio_pause = has_audio_stream
            && previous_state != CastProgramClockState::idle
            && previous_state != CastProgramClockState::paused;
        return update;
    }

    if (input.has_video
            && input.video_buffer.availability
                == CastVideoBufferAvailability::fatal_error) {
        state_ = CastProgramClockState::fatal_error;
        anchor(position_seconds_, now);
        auto update = result();
        update.request_audio_pause = has_audio_stream
            && previous_state != CastProgramClockState::fatal_error;
        return update;
    }

    if (state_ == CastProgramClockState::ended
            || state_ == CastProgramClockState::fatal_error) {
        return result();
    }

    // Catalog songs use a separate vocal/accompaniment track as their master
    // clock. Their background video is allowed to be longer, shorter or loop-
    // padded, so waiting for video EOS after the audio backend has naturally
    // ended can trap the clock in a playing/underflow cycle forever. Use the
    // backend's explicit EOF signal rather than a position~=duration guess;
    // pauses caused by video gating therefore cannot end the song.
    if (input.end_on_audio_eof && input.has_audio_clock
            && input.audio_end_of_stream) {
        const auto audio_end = finite_non_negative(
            input.audio_duration_seconds);
        position_seconds_ = audio_end > 0.0
            ? audio_end : std::max(position_seconds_, audio_position);
        state_ = CastProgramClockState::ended;
        anchor(position_seconds_, now);
        auto update = result();
        update.request_audio_pause = true;
        return update;
    }

    if (state_ == CastProgramClockState::paused) {
        // A user pause preserves both the decoder identity and its queued
        // frames. Resuming from a still-covered position only needs the
        // low-water safety margin; imposing the cold-start high-water gate
        // here can strand a paused progressive source in priming forever.
        const bool resume_is_covered = !input.has_video
            || hold_video_tail_for_audio
            || !cast_video_buffer_below_low_water(
                input.video_buffer, position_seconds_, watermarks_);
        if (resume_is_covered) {
            state_ = CastProgramClockState::playing;
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
            ? CastProgramClockState::priming
            : CastProgramClockState::playing;
        anchor(position_seconds_, now);
    } else if (state_ == CastProgramClockState::idle) {
        state_ = input.has_video
            ? CastProgramClockState::priming
            : CastProgramClockState::playing;
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

    if (state_ == CastProgramClockState::priming
            || state_ == CastProgramClockState::underflow) {
        if (input.has_video && !hold_video_tail_for_audio
                && !cast_video_buffer_reached_high_water(
                    input.video_buffer, position_seconds_, watermarks_)) {
            auto update = result();
            update.request_audio_pause = has_audio_stream
                && previous_state != CastProgramClockState::priming
                && previous_state != CastProgramClockState::underflow;
            return update;
        }

        state_ = CastProgramClockState::playing;
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
        // Audio backends can briefly report their pre-seek cursor while a
        // resume is being applied. Only an explicit discontinuity may rewind
        // the program clock.
        candidate = std::max(position_seconds_, audio_position);
    } else {
        candidate = anchor_position_seconds_
            + std::max(0.0, now - anchor_monotonic_seconds_);
    }

    if (input.has_video && !input.end_on_audio_eof) {
        const auto end = effective_video_end(input.video_buffer);
        const bool has_declared_duration = finite_non_negative(
            input.video_buffer.source_duration_seconds) > 0.0;
        const bool decoder_reached_eos = input.video_buffer.availability
            == CastVideoBufferAvailability::end_of_stream;
        // MF can expose a complete progressive HTTP presentation and its
        // duration before the underlying response is closed. The declared
        // presentation duration is already authoritative in that case; do
        // not require a delayed transport-level EOS before ending the program.
        // Sources without a duration retain the old EOS + buffered-tail rule.
        if ((has_declared_duration || decoder_reached_eos)
                && end > 0.0
                && candidate + watermarks_.timestamp_tolerance_seconds
                    >= end) {
            position_seconds_ = end;
            state_ = CastProgramClockState::ended;
            anchor(position_seconds_, now);
            auto update = result();
            update.request_audio_pause = has_audio_stream;
            return update;
        }
    }

    if (input.has_video && !hold_video_tail_for_audio
            && cast_video_buffer_below_low_water(
            input.video_buffer, candidate, watermarks_)) {
        if (cast_video_buffer_contains(
                input.video_buffer, candidate,
                watermarks_.timestamp_tolerance_seconds)) {
            position_seconds_ = candidate;
        }
        state_ = CastProgramClockState::underflow;
        anchor(position_seconds_, now);
        auto update = result();
        update.request_audio_pause = has_audio_stream;
        return update;
    }

    position_seconds_ = candidate;
    return result();
}

double CastProgramClock::position_seconds() const {
    std::lock_guard lock(mutex_);
    return position_seconds_;
}

CastProgramClockState CastProgramClock::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

void CastProgramClock::anchor(
        double position_seconds, double monotonic_seconds) noexcept {
    position_seconds_ = finite_non_negative(position_seconds);
    anchor_position_seconds_ = position_seconds_;
    anchor_monotonic_seconds_ = std::isfinite(monotonic_seconds)
        ? monotonic_seconds : 0.0;
}

CastProgramClockUpdate CastProgramClock::result() const noexcept {
    CastProgramClockUpdate update;
    update.position_seconds = position_seconds_;
    update.state = state_;
    update.audio_seek_position_seconds = position_seconds_;
    return update;
}
