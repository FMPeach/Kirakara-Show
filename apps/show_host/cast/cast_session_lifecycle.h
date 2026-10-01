#pragma once

#include "cast_program_clock.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>

enum class CastSessionLifecycleState : std::uint32_t {
    stopped,
    starting,
    priming,
    playing,
    paused,
    underflow,
    switching,
    ended,
    failed,
};

enum class CastSessionLifecycleReason : std::uint32_t {
    none,
    host_start,
    pipeline_ready,
    program_change,
    program_load_failed,
    play_requested,
    pause_requested,
    video_starvation,
    video_recovered,
    program_frame_committed,
    media_ended,
    decoder_failure,
    host_stop,
};

struct CastSessionLifecycleSnapshot {
    CastSessionLifecycleState state{CastSessionLifecycleState::stopped};
    CastSessionLifecycleReason reason{CastSessionLifecycleReason::none};
    bool transport_active{};
    bool program_ready{};
    bool holding_previous_output{};
    bool start_gate_pending{};
    std::uint64_t ready_timeline_revision{};
    double start_position_seconds{};
    std::chrono::steady_clock::time_point start_gate_started{};

    [[nodiscard]] bool buffering() const noexcept {
        return state == CastSessionLifecycleState::priming
            || state == CastSessionLifecycleState::underflow;
    }
};

// Cast transport lifetime, visual-program readiness and the program clock are
// related but independent dimensions. This small state machine owns their
// transitions without owning rendering, decoding or the host output lease.
class CastSessionLifecycle {
public:
    void begin_start();
    void activate_transport();
    void stop();

    void begin_program_transition(bool preserve_previous_output);
    void fail_program_transition();
    void prepare_program(
        bool wait_for_first_frame,
        double position_seconds,
        std::chrono::steady_clock::time_point now,
        bool playback_requested);
    void request_play(
        bool wait_for_first_frame,
        double position_seconds,
        std::chrono::steady_clock::time_point now);
    void request_pause();
    void reset_program();

    void apply_clock_state(CastProgramClockState state);
    void mark_program_frame_committed(std::uint64_t timeline_revision);
    void set_previous_output_hold(bool holding);
    void clear_start_gate();
    void suppress_start_gate_warning();

    [[nodiscard]] bool program_ready_for(
        std::uint64_t timeline_revision) const;
    [[nodiscard]] bool should_notify_ended(
        std::uint64_t timeline_revision);
    [[nodiscard]] bool should_notify_fatal(
        std::uint64_t timeline_revision);
    [[nodiscard]] CastSessionLifecycleSnapshot snapshot() const;

private:
    void set_state(
        CastSessionLifecycleState state,
        CastSessionLifecycleReason reason) noexcept;
    void arm_start_gate(
        bool pending,
        double position_seconds,
        std::chrono::steady_clock::time_point now) noexcept;
    void clear_start_gate_unlocked() noexcept;
    void reset_program_unlocked() noexcept;

    mutable std::mutex mutex_;
    CastSessionLifecycleSnapshot state_;
    std::uint64_t ended_notification_revision_{
        std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t fatal_notification_revision_{
        std::numeric_limits<std::uint64_t>::max()};
};
