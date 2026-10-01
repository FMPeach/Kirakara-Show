#pragma once

#include "cast_video_buffer_state.h"

#include <cstdint>
#include <mutex>

enum class CastProgramClockState : std::uint32_t {
    idle,
    priming,
    playing,
    underflow,
    paused,
    ended,
    fatal_error,
};

struct CastProgramClockInput {
    double monotonic_seconds{};
    bool playback_requested{};
    bool has_video{};
    // Audio may be present without being the program clock. Video-master
    // Cast uses the monotonic clock but still needs pause/seek/play commands
    // so its separate DASH audio remains slaved to that timeline.
    bool has_audio_stream{};
    bool has_audio_clock{};
    double audio_position_seconds{};
    bool end_on_audio_eof{};
    bool audio_end_of_stream{};
    double audio_duration_seconds{};
    CastVideoBufferSnapshot video_buffer;
};

struct CastProgramClockUpdate {
    double position_seconds{};
    CastProgramClockState state{CastProgramClockState::idle};
    bool request_audio_pause{};
    bool request_audio_seek{};
    bool request_audio_play{};
    double audio_seek_position_seconds{};
};

// Cast owns this clock only while the TS output lease is active. It consumes
// either an audio position or a monotonic anchor, but video buffer watermarks
// gate advancement in both cases. It never reads MediaEngine currentTime.
class CastProgramClock {
public:
    explicit CastProgramClock(
        CastVideoBufferWatermarks watermarks = {}) noexcept;

    void reset(double position_seconds, double monotonic_seconds);
    void discontinuity(
        double position_seconds, double monotonic_seconds);
    void pause_at(
        double position_seconds, double monotonic_seconds);

    [[nodiscard]] CastProgramClockUpdate update(
        const CastProgramClockInput& input);

    [[nodiscard]] double position_seconds() const;
    [[nodiscard]] CastProgramClockState state() const;

private:
    void anchor(double position_seconds, double monotonic_seconds) noexcept;
    [[nodiscard]] CastProgramClockUpdate result() const noexcept;

    CastVideoBufferWatermarks watermarks_;
    mutable std::mutex mutex_;
    CastProgramClockState state_{CastProgramClockState::idle};
    double position_seconds_{};
    double anchor_position_seconds_{};
    double anchor_monotonic_seconds_{};
};
