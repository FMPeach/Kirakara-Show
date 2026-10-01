#include "../apps/show_host/stage/native_stage_pipeline_diagnostics.h"

#include <cstdlib>
#include <iostream>

namespace {

void expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "native stage diagnostics test failed: " << message << '\n';
    std::exit(1);
}

}  // namespace

int main() {
    NativeStagePipelineDiagnostics diagnostics;
    diagnostics.set_enabled(false);
    diagnostics.record_duration_ticks(
        NativeStagePipelineEvent::stage_tick, 100);
    diagnostics.increment(NativeStagePipelineCounter::render_ticks);
    auto snapshot = diagnostics.snapshot();
    expect(snapshot.event(NativeStagePipelineEvent::stage_tick).count == 0,
        "disabled duration recording was not inert");
    expect(snapshot.counter(NativeStagePipelineCounter::render_ticks) == 0,
        "disabled counter recording was not inert");

    diagnostics.set_enabled(true);
    const auto frequency = diagnostics.snapshot().qpc_frequency;
    expect(frequency != 0, "QPC frequency was unavailable");
    const auto one_ms = frequency / 1000;
    const auto ten_ms = frequency / 100;
    expect(one_ms != 0 && ten_ms != 0, "QPC test durations rounded to zero");

    for (int index = 0; index < 95; ++index) {
        diagnostics.record_duration_ticks(
            NativeStagePipelineEvent::stage_tick, one_ms);
    }
    for (int index = 0; index < 5; ++index) {
        diagnostics.record_duration_ticks(
            NativeStagePipelineEvent::stage_tick, ten_ms);
    }
    diagnostics.increment(
        NativeStagePipelineCounter::render_ticks, 100);
    diagnostics.increment(
        NativeStagePipelineCounter::repeated_video_frames, 47);
    diagnostics.increment(
        NativeStagePipelineCounter::static_frame_skips, 53);
    diagnostics.increment(
        NativeStagePipelineCounter::d2d_flush_boundaries, 7);
    diagnostics.increment(
        NativeStagePipelineCounter::d3d_flush_boundaries, 3);
    diagnostics.record_duration_ticks(
        NativeStagePipelineEvent::standby_prime, ten_ms);
    diagnostics.increment(
        NativeStagePipelineCounter::standby_promotions, 2);
    diagnostics.increment(
        NativeStagePipelineCounter::preview_degraded_to_20, 1);

    snapshot = diagnostics.snapshot();
    const auto& ticks = snapshot.event(NativeStagePipelineEvent::stage_tick);
    expect(ticks.count == 100, "duration count mismatch");
    expect(ticks.total_duration_qpc == one_ms * 95 + ten_ms * 5,
        "duration total mismatch");
    expect(ticks.maximum_duration_qpc == ten_ms,
        "duration maximum mismatch");
    expect(ticks.p95_duration_qpc >= one_ms
            && ticks.p95_duration_qpc <= one_ms + frequency / 2000,
        "histogram P95 did not select the 1ms population");
    expect(snapshot.counter(NativeStagePipelineCounter::render_ticks) == 100,
        "render tick counter mismatch");
    expect(snapshot.counter(
            NativeStagePipelineCounter::repeated_video_frames) == 47,
        "repeated frame counter mismatch");
    expect(snapshot.counter(
            NativeStagePipelineCounter::static_frame_skips) == 53,
        "static frame skip counter mismatch");
    expect(snapshot.counter(
            NativeStagePipelineCounter::d2d_flush_boundaries) == 7,
        "D2D boundary counter mismatch");
    expect(snapshot.counter(
            NativeStagePipelineCounter::d3d_flush_boundaries) == 3,
        "D3D boundary counter mismatch");
    expect(snapshot.event(
            NativeStagePipelineEvent::standby_prime).count == 1,
        "standby prime duration count mismatch");
    expect(snapshot.counter(
            NativeStagePipelineCounter::standby_promotions) == 2,
        "standby promotion counter mismatch");
    expect(snapshot.counter(
            NativeStagePipelineCounter::preview_degraded_to_20) == 1,
        "adaptive Preview transition counter mismatch");

    diagnostics.reset_interval();
    snapshot = diagnostics.snapshot();
    expect(snapshot.event(NativeStagePipelineEvent::stage_tick).count == 0,
        "interval reset retained event samples");
    expect(snapshot.counter(NativeStagePipelineCounter::render_ticks) == 0,
        "interval reset retained counters");

    std::cout << "Native Stage pipeline diagnostics tests passed.\n";
    return 0;
}
