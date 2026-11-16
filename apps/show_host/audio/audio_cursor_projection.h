#pragma once

#include "audio_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

inline constexpr double kMaxAudioPictureOffsetSeconds = 1.0 / 60.0;

struct AudioCursorReading {
    double wall_seconds{};
    double raw_seconds{};
    double duration_seconds{};
    std::uint64_t timeline_revision{};
    bool device_running{};
    bool from_cache{};
    std::uint64_t running_epoch{};
};

// Used only for presentation. Each output evaluates it on its existing
// render tick; no playback timer or decoder directive is driven by it.
class AudioCursorProjection {
public:
    void reset() noexcept { has_anchor_ = false; }

    double observe(const AudioCursorReading& reading) noexcept {
        const bool same_timeline = has_anchor_
            && revision_ == reading.timeline_revision
            && running_epoch_ == reading.running_epoch;
        const auto duration = std::isfinite(reading.duration_seconds)
                && reading.duration_seconds > 0.0
            ? reading.duration_seconds
            : std::numeric_limits<double>::infinity();
        const bool valid_cursor =
            std::isfinite(reading.raw_seconds) && reading.raw_seconds >= 0.0;
        const auto raw = std::min(duration, valid_cursor
            ? reading.raw_seconds : (same_timeline ? raw_ : 0.0));
        if (!valid_cursor || !std::isfinite(reading.wall_seconds)) {
            has_anchor_ = false;
            return raw;
        }

        if (!same_timeline || !reading.device_running || duration != duration_) {
            return anchor(reading, raw, duration);
        }
        const auto elapsed = reading.wall_seconds - wall_;
        if (elapsed < 0.0 || elapsed > 0.250) {
            return anchor(reading, reading.from_cache ? raw_ : raw, duration);
        }
        wall_ = reading.wall_seconds;

        // A quantized read can move a few ticks at once. Larger unexplained
        // jumps re-anchor a direct read; an old cache entry cannot reset it.
        const bool large_jump = raw < raw_ - 0.050
            || raw > raw_ + elapsed + 0.050;
        if (large_jump && !reading.from_cache) {
            return anchor(reading, raw, duration);
        }
        const bool moved = !large_jump && raw > raw_;
        if (moved) raw_ = raw;

        // In the last frame's worth of audio, use the real cursor only.
        const auto tail = duration - kMaxAudioPictureOffsetSeconds;
        if (raw_ >= tail || !projecting_) {
            projecting_ = raw_ < tail && moved && !reading.from_cache;
            return position_ = raw_;
        }

        auto predicted = position_ + elapsed;
        if (moved && !reading.from_cache) {
            // Ignore a whole observed cursor quantum. Outside it, correct
            // slowly so hardware clock drift does not keep hitting the clamp.
            constexpr double deadband = 0.010;
            constexpr double settle_seconds = 1.0;
            const auto error = raw_ - predicted;
            const auto excess = std::max(0.0, std::abs(error) - deadband);
            predicted += std::copysign(excess, error)
                * elapsed / (settle_seconds + elapsed);
        }
        // A stale cursor holds us at the upper bound until there is new
        // evidence of progress.
        const auto lo = std::max(0.0, raw_ - kMaxAudioPictureOffsetSeconds);
        const auto hi = std::min(tail, raw_ + kMaxAudioPictureOffsetSeconds);
        position_ = std::max(position_, std::clamp(predicted, lo, hi));
        return position_;
    }

private:
    double anchor(
            const AudioCursorReading& reading,
            double raw,
            double duration) noexcept {
        has_anchor_ = true;
        projecting_ = false;
        revision_ = reading.timeline_revision;
        running_epoch_ = reading.running_epoch;
        wall_ = reading.wall_seconds;
        duration_ = duration;
        raw_ = position_ = raw;
        return raw;
    }

    bool has_anchor_{};
    bool projecting_{};
    std::uint64_t revision_{};
    std::uint64_t running_epoch_{};
    double wall_{};
    double duration_{};
    double raw_{};
    double position_{};
};

[[nodiscard]] inline double audio_picture_time(
        AudioCursorProjection& projection,
        const AudioPositionSample& sample,
        double monotonic_seconds,
        double duration_seconds,
        double audio_clock_offset_seconds,
        double raw_program_seconds,
        std::uint64_t timeline_revision,
        bool steady_playback) noexcept {
    // If the program clock uses a held position instead of the audio sample
    // (e.g. the old cursor after a seek), it has authority over the picture too.
    if (!steady_playback || sample.running_epoch == 0
            || !std::isfinite(sample.seconds)
            || std::max(0.0, sample.seconds + audio_clock_offset_seconds)
                != raw_program_seconds) {
        projection.reset();
        return raw_program_seconds;
    }
    const auto position = projection.observe({
        monotonic_seconds, sample.seconds, duration_seconds, timeline_revision,
        true, sample.from_cache, sample.running_epoch,
    });
    const auto picture = std::max(0.0, position + audio_clock_offset_seconds);
    if (std::abs(picture - raw_program_seconds)
            > kMaxAudioPictureOffsetSeconds + 1e-9) {
        projection.reset();
        return raw_program_seconds;
    }
    return picture;
}
