#include "../apps/show_host/cast/cast_session_lifecycle.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

void expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

}  // namespace

int main() {
    using namespace std::chrono_literals;

    CastSessionLifecycle lifecycle;
    auto state = lifecycle.snapshot();
    expect(state.state == CastSessionLifecycleState::stopped,
        "new Cast lifecycle was not stopped");
    expect(!state.transport_active, "stopped lifecycle retained transport");

    lifecycle.begin_start();
    state = lifecycle.snapshot();
    expect(state.state == CastSessionLifecycleState::starting,
        "start did not enter starting");
    lifecycle.activate_transport();
    state = lifecycle.snapshot();
    expect(state.transport_active, "ready pipeline did not own transport");
    expect(state.state == CastSessionLifecycleState::paused,
        "ready idle pipeline was not paused");

    lifecycle.begin_program_transition(true);
    state = lifecycle.snapshot();
    expect(state.state == CastSessionLifecycleState::switching,
        "program change did not enter switching");
    expect(state.holding_previous_output,
        "seamless transition discarded the previous program");

    const auto gate_time = std::chrono::steady_clock::time_point{5s};
    lifecycle.prepare_program(true, 12.5, gate_time, true);
    state = lifecycle.snapshot();
    expect(state.state == CastSessionLifecycleState::priming,
        "video program did not enter priming");
    expect(state.buffering(), "priming was not reported as buffering");
    expect(state.start_gate_pending, "video program did not arm start gate");
    expect(state.start_position_seconds == 12.5,
        "start gate lost requested position");

    lifecycle.apply_clock_state(CastProgramClockState::underflow);
    state = lifecycle.snapshot();
    expect(state.state == CastSessionLifecycleState::underflow,
        "starvation did not enter underflow");
    expect(state.buffering(), "underflow was not reported as buffering");

    lifecycle.apply_clock_state(CastProgramClockState::playing);
    state = lifecycle.snapshot();
    expect(state.state == CastSessionLifecycleState::playing,
        "buffer recovery did not enter playing");
    expect(state.reason == CastSessionLifecycleReason::video_recovered,
        "buffer recovery lost its transition reason");

    lifecycle.mark_program_frame_committed(42);
    state = lifecycle.snapshot();
    expect(state.program_ready && lifecycle.program_ready_for(42),
        "committed frame did not publish its timeline revision");
    expect(!state.holding_previous_output,
        "committed frame did not release seamless hold");
    lifecycle.clear_start_gate();
    expect(!lifecycle.snapshot().start_gate_pending,
        "committed frame did not release start gate");

    lifecycle.request_pause();
    state = lifecycle.snapshot();
    expect(state.state == CastSessionLifecycleState::paused,
        "pause did not enter paused");
    expect(!state.buffering(), "paused state was also buffering");
    expect(!state.start_gate_pending, "pause retained start gate");

    lifecycle.request_play(false, 12.5, gate_time);
    expect(lifecycle.snapshot().state == CastSessionLifecycleState::playing,
        "covered resume unnecessarily entered priming");
    lifecycle.apply_clock_state(CastProgramClockState::ended);
    expect(lifecycle.snapshot().state == CastSessionLifecycleState::ended,
        "program EOF did not enter ended");
    expect(lifecycle.should_notify_ended(42),
        "first ended notification was suppressed");
    expect(!lifecycle.should_notify_ended(42),
        "ended notification repeated for one timeline");
    expect(lifecycle.should_notify_ended(43),
        "new timeline did not receive an ended notification");

    lifecycle.apply_clock_state(CastProgramClockState::fatal_error);
    expect(lifecycle.snapshot().state == CastSessionLifecycleState::failed,
        "decoder failure did not enter failed");
    expect(lifecycle.should_notify_fatal(43),
        "first fatal notification was suppressed");
    expect(!lifecycle.should_notify_fatal(43),
        "fatal notification repeated for one timeline");

    lifecycle.fail_program_transition();
    state = lifecycle.snapshot();
    expect(state.state == CastSessionLifecycleState::paused,
        "failed load left an illegal terminal session state");
    expect(!state.program_ready && !state.holding_previous_output,
        "failed load retained stale visual readiness");

    lifecycle.stop();
    state = lifecycle.snapshot();
    expect(state.state == CastSessionLifecycleState::stopped,
        "host stop did not enter stopped");
    expect(!state.transport_active && !state.buffering(),
        "stopped lifecycle retained active dimensions");

    CastSessionLifecycle concurrent_lifecycle;
    concurrent_lifecycle.begin_start();
    concurrent_lifecycle.activate_transport();
    std::thread controller([&] {
        for (std::uint64_t revision = 1; revision <= 2000; ++revision) {
            concurrent_lifecycle.begin_program_transition(true);
            concurrent_lifecycle.mark_program_frame_committed(revision);
        }
    });
    for (std::uint64_t revision = 1; revision <= 2000; ++revision) {
        static_cast<void>(concurrent_lifecycle.snapshot());
        static_cast<void>(
            concurrent_lifecycle.program_ready_for(revision));
        concurrent_lifecycle.apply_clock_state(
            CastProgramClockState::playing);
    }
    controller.join();
    concurrent_lifecycle.stop();
    expect(concurrent_lifecycle.snapshot().state
            == CastSessionLifecycleState::stopped,
        "concurrent lifecycle access did not remain operable");
    return 0;
}
