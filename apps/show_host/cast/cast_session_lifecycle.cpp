#include "cast_session_lifecycle.h"

#include <algorithm>
#include <cmath>

void CastSessionLifecycle::set_state(
        CastSessionLifecycleState state,
        CastSessionLifecycleReason reason) noexcept {
    state_.state = state;
    state_.reason = reason;
}

void CastSessionLifecycle::arm_start_gate(
        bool pending,
        double position_seconds,
        std::chrono::steady_clock::time_point now) noexcept {
    state_.start_gate_pending = pending;
    state_.start_position_seconds = pending && std::isfinite(position_seconds)
        ? std::max(0.0, position_seconds) : 0.0;
    state_.start_gate_started = pending
        ? now : std::chrono::steady_clock::time_point{};
}

void CastSessionLifecycle::begin_start() {
    std::lock_guard lock(mutex_);
    state_ = {};
    set_state(CastSessionLifecycleState::starting,
        CastSessionLifecycleReason::host_start);
}

void CastSessionLifecycle::activate_transport() {
    std::lock_guard lock(mutex_);
    state_.transport_active = true;
    set_state(CastSessionLifecycleState::paused,
        CastSessionLifecycleReason::pipeline_ready);
}

void CastSessionLifecycle::stop() {
    std::lock_guard lock(mutex_);
    state_ = {};
    set_state(CastSessionLifecycleState::stopped,
        CastSessionLifecycleReason::host_stop);
}

void CastSessionLifecycle::begin_program_transition(
        bool preserve_previous_output) {
    std::lock_guard lock(mutex_);
    if (!state_.transport_active) {
        reset_program_unlocked();
        return;
    }
    state_.holding_previous_output = preserve_previous_output;
    if (!preserve_previous_output) {
        state_.program_ready = false;
        state_.ready_timeline_revision = 0;
    }
    clear_start_gate_unlocked();
    set_state(CastSessionLifecycleState::switching,
        CastSessionLifecycleReason::program_change);
}

void CastSessionLifecycle::fail_program_transition() {
    std::lock_guard lock(mutex_);
    state_.holding_previous_output = false;
    state_.program_ready = false;
    state_.ready_timeline_revision = 0;
    clear_start_gate_unlocked();
    set_state(state_.transport_active
            ? CastSessionLifecycleState::paused
            : CastSessionLifecycleState::stopped,
        CastSessionLifecycleReason::program_load_failed);
}

void CastSessionLifecycle::prepare_program(
        bool wait_for_first_frame,
        double position_seconds,
        std::chrono::steady_clock::time_point now,
        bool playback_requested) {
    std::lock_guard lock(mutex_);
    if (!state_.transport_active) return;
    arm_start_gate(wait_for_first_frame, position_seconds, now);
    if (!playback_requested) {
        set_state(CastSessionLifecycleState::paused,
            CastSessionLifecycleReason::pipeline_ready);
    } else if (wait_for_first_frame) {
        set_state(CastSessionLifecycleState::priming,
            CastSessionLifecycleReason::play_requested);
    } else {
        set_state(CastSessionLifecycleState::playing,
            CastSessionLifecycleReason::play_requested);
    }
}

void CastSessionLifecycle::request_play(
        bool wait_for_first_frame,
        double position_seconds,
        std::chrono::steady_clock::time_point now) {
    std::lock_guard lock(mutex_);
    if (!state_.transport_active) return;
    arm_start_gate(wait_for_first_frame, position_seconds, now);
    set_state(wait_for_first_frame
            ? CastSessionLifecycleState::priming
            : CastSessionLifecycleState::playing,
        CastSessionLifecycleReason::play_requested);
}

void CastSessionLifecycle::request_pause() {
    std::lock_guard lock(mutex_);
    if (!state_.transport_active) return;
    clear_start_gate_unlocked();
    set_state(CastSessionLifecycleState::paused,
        CastSessionLifecycleReason::pause_requested);
}

void CastSessionLifecycle::reset_program() {
    std::lock_guard lock(mutex_);
    reset_program_unlocked();
}

void CastSessionLifecycle::reset_program_unlocked() noexcept {
    state_.holding_previous_output = false;
    state_.program_ready = false;
    state_.ready_timeline_revision = 0;
    clear_start_gate_unlocked();
    set_state(state_.transport_active
            ? CastSessionLifecycleState::paused
            : CastSessionLifecycleState::stopped,
        CastSessionLifecycleReason::host_stop);
}

void CastSessionLifecycle::apply_clock_state(
        CastProgramClockState state) {
    std::lock_guard lock(mutex_);
    switch (state) {
    case CastProgramClockState::idle:
        if (state_.state != CastSessionLifecycleState::switching) {
            set_state(CastSessionLifecycleState::paused,
                CastSessionLifecycleReason::pipeline_ready);
        }
        break;
    case CastProgramClockState::priming:
        set_state(CastSessionLifecycleState::priming,
            CastSessionLifecycleReason::play_requested);
        break;
    case CastProgramClockState::playing:
        set_state(CastSessionLifecycleState::playing,
            state_.state == CastSessionLifecycleState::underflow
                ? CastSessionLifecycleReason::video_recovered
                : CastSessionLifecycleReason::play_requested);
        break;
    case CastProgramClockState::underflow:
        set_state(CastSessionLifecycleState::underflow,
            CastSessionLifecycleReason::video_starvation);
        break;
    case CastProgramClockState::paused:
        set_state(CastSessionLifecycleState::paused,
            CastSessionLifecycleReason::pause_requested);
        break;
    case CastProgramClockState::ended:
        clear_start_gate_unlocked();
        set_state(CastSessionLifecycleState::ended,
            CastSessionLifecycleReason::media_ended);
        break;
    case CastProgramClockState::fatal_error:
        clear_start_gate_unlocked();
        set_state(CastSessionLifecycleState::failed,
            CastSessionLifecycleReason::decoder_failure);
        break;
    }
}

void CastSessionLifecycle::mark_program_frame_committed(
        std::uint64_t timeline_revision) {
    std::lock_guard lock(mutex_);
    state_.program_ready = true;
    state_.ready_timeline_revision = timeline_revision;
    state_.holding_previous_output = false;
    state_.reason = CastSessionLifecycleReason::program_frame_committed;
}

void CastSessionLifecycle::set_previous_output_hold(bool holding) {
    std::lock_guard lock(mutex_);
    state_.holding_previous_output = holding;
}

void CastSessionLifecycle::clear_start_gate() {
    std::lock_guard lock(mutex_);
    clear_start_gate_unlocked();
}

void CastSessionLifecycle::clear_start_gate_unlocked() noexcept {
    arm_start_gate(false, 0.0, {});
}

void CastSessionLifecycle::suppress_start_gate_warning() {
    std::lock_guard lock(mutex_);
    state_.start_gate_started = {};
}

bool CastSessionLifecycle::program_ready_for(
        std::uint64_t timeline_revision) const {
    std::lock_guard lock(mutex_);
    return state_.program_ready
        && state_.ready_timeline_revision == timeline_revision;
}

bool CastSessionLifecycle::should_notify_ended(
        std::uint64_t timeline_revision) {
    std::lock_guard lock(mutex_);
    if (ended_notification_revision_ == timeline_revision) return false;
    ended_notification_revision_ = timeline_revision;
    return true;
}

bool CastSessionLifecycle::should_notify_fatal(
        std::uint64_t timeline_revision) {
    std::lock_guard lock(mutex_);
    if (fatal_notification_revision_ == timeline_revision) return false;
    fatal_notification_revision_ = timeline_revision;
    return true;
}

CastSessionLifecycleSnapshot CastSessionLifecycle::snapshot() const {
    std::lock_guard lock(mutex_);
    return state_;
}
