#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

constexpr double kCastMaxAvDeltaSeconds = 0.050;
constexpr double kCastAudioRealignSeconds = 0.025;

[[nodiscard]] inline bool cast_program_ready_for_timeline(
        bool program_ready,
        std::uint64_t ready_timeline_revision,
        std::uint64_t current_timeline_revision) noexcept {
    return program_ready
        && ready_timeline_revision == current_timeline_revision;
}

// Shared by both outputs: a decoded sample remains the correct picture for its
// declared duration.  Keep the historical 50 ms tolerance as the minimum for
// ordinary 30/60 fps sources and short decoder jitter, but do not reject a
// legitimate low-frame-rate/VFR sample before its own presentation interval
// ends.
[[nodiscard]] inline bool video_frame_matches_program_time(
        double frame_time_seconds,
        double frame_duration_seconds,
        double program_time_seconds) noexcept {
    if (!std::isfinite(frame_time_seconds)
            || !std::isfinite(program_time_seconds)) {
        return false;
    }
    const auto valid_duration = std::isfinite(frame_duration_seconds)
            && frame_duration_seconds > 0.0
        ? frame_duration_seconds : 0.0;
    const auto delta = program_time_seconds - frame_time_seconds;
    return delta >= -kCastMaxAvDeltaSeconds - 1e-9
        && delta <= std::max(kCastMaxAvDeltaSeconds, valid_duration) + 1e-9;
}

[[nodiscard]] inline double cast_audio_chunk_start_time(
        double current_program_time_seconds,
        std::uint64_t skipped_video_frames,
        std::uint32_t frame_rate_num,
        std::uint32_t frame_rate_den) noexcept {
    if (!std::isfinite(current_program_time_seconds)
            || frame_rate_num == 0
            || frame_rate_den == 0) {
        return 0.0;
    }
    const auto skipped_seconds = static_cast<double>(skipped_video_frames)
        * static_cast<double>(frame_rate_den)
        / static_cast<double>(frame_rate_num);
    return std::max(0.0, current_program_time_seconds - skipped_seconds);
}
