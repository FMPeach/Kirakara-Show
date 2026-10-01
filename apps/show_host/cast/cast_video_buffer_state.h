#pragma once

#include <cstddef>
#include <cstdint>

enum class CastVideoBufferAvailability : std::uint32_t {
    idle,
    opening,
    ready,
    retryable_starvation,
    end_of_stream,
    fatal_error,
};

struct CastVideoBufferSnapshot {
    std::size_t frame_count{};
    double start_seconds{};
    double end_seconds{};
    double source_duration_seconds{};
    CastVideoBufferAvailability availability{
        CastVideoBufferAvailability::idle};
};

struct CastVideoBufferWatermarks {
    // Low and high are deliberately separated. Without hysteresis a 60 fps
    // source can alternate between pause/resume on adjacent decoder samples.
    double low_seconds{0.033};
    double high_seconds{0.120};
    double timestamp_tolerance_seconds{1.0 / 240.0};
    std::size_t minimum_start_frames{1};
};

[[nodiscard]] double cast_video_buffer_runway(
    const CastVideoBufferSnapshot& buffer,
    double position_seconds) noexcept;

[[nodiscard]] bool cast_video_buffer_contains(
    const CastVideoBufferSnapshot& buffer,
    double position_seconds,
    double tolerance_seconds = 1.0 / 240.0) noexcept;

[[nodiscard]] bool cast_video_buffer_below_low_water(
    const CastVideoBufferSnapshot& buffer,
    double position_seconds,
    const CastVideoBufferWatermarks& watermarks = {}) noexcept;

[[nodiscard]] bool cast_video_buffer_reached_high_water(
    const CastVideoBufferSnapshot& buffer,
    double position_seconds,
    const CastVideoBufferWatermarks& watermarks = {}) noexcept;
