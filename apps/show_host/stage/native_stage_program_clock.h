#pragma once

#include "../media/mf_d3d11_video_decoder.h"

#include <cstddef>
#include <cstdint>
#include <mutex>

enum class NativeStageProgramClockState : std::uint32_t {
    idle,
    priming,
    playing,
    buffering,
    paused,
    ended,
    fatal_error,
};

struct NativeStageVideoBufferSnapshot {
    std::size_t frame_count{};
    double start_seconds{};
    double end_seconds{};
    double source_duration_seconds{};
    MfD3D11VideoAvailability availability{
        MfD3D11VideoAvailability::idle};
    MfD3D11VideoSourceBufferingState source_buffering_state{
        MfD3D11VideoSourceBufferingState::unknown};
};

struct NativeStageProgramClockConfig {
    // Underflow is entered only after the current time is no longer covered.
    // This runway is therefore an exit hysteresis, never an early-pause gate.
    double recovery_runway_seconds{0.100};
    // The Native BGRA queue is intentionally shallow to cap 4K memory. Very
    // high-frame-rate sources can fill that queue before spanning 100 ms, so
    // a full historical five-frame queue is also a valid recovery runway.
    std::size_t minimum_recovery_frames{5};
    double timestamp_tolerance_seconds{1.0 / 240.0};
};

struct NativeStageProgramClockInput {
    double monotonic_seconds{};
    bool playback_requested{};
    bool has_video{};
    bool has_audio_stream{};
    bool has_audio_clock{};
    double audio_position_seconds{};
    bool end_on_audio_eof{};
    bool audio_end_of_stream{};
    double audio_duration_seconds{};
    NativeStageVideoBufferSnapshot video_buffer;
};

struct NativeStageProgramClockUpdate {
    double position_seconds{};
    NativeStageProgramClockState state{NativeStageProgramClockState::idle};
    bool request_audio_pause{};
    bool request_audio_seek{};
    bool request_audio_play{};
    double audio_seek_position_seconds{};
};

// Native Stage owns this clock only while a native output is active. The
// decoder reports coverage and gates advancement, but never selects the clock
// source: catalog media remains audio-mastered while external media uses a
// monotonic video timeline. This mirrors Cast's proven semantics without
// coupling Native output to Cast transport/session state.
class NativeStageProgramClock {
public:
    explicit NativeStageProgramClock(
        NativeStageProgramClockConfig config = {}) noexcept;

    void reset(double position_seconds, double monotonic_seconds);
    void discontinuity(double position_seconds, double monotonic_seconds);
    void pause_at(double position_seconds, double monotonic_seconds);

    [[nodiscard]] NativeStageProgramClockUpdate update(
        const NativeStageProgramClockInput& input);

    [[nodiscard]] double position_seconds() const;
    [[nodiscard]] NativeStageProgramClockState state() const;

private:
    void anchor(double position_seconds, double monotonic_seconds) noexcept;
    [[nodiscard]] NativeStageProgramClockUpdate result() const noexcept;

    NativeStageProgramClockConfig config_;
    mutable std::mutex mutex_;
    NativeStageProgramClockState state_{NativeStageProgramClockState::idle};
    // A user pause can interrupt either normal playback or an internal
    // priming/buffering wait. Preserve that distinction so resume cannot
    // briefly restart audio against a queue that still lacks runway.
    bool paused_requires_video_runway_{};
    double position_seconds_{};
    double anchor_position_seconds_{};
    double anchor_monotonic_seconds_{};
};

[[nodiscard]] NativeStageVideoBufferSnapshot native_stage_buffer_snapshot(
    const MfD3D11VideoBufferStatus& status) noexcept;
