#include "../apps/show_host/cast/cast_pipeline_diagnostics.h"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}  // namespace

int main() {
    CastPipelineDiagnostics diagnostics;
    const auto first_generation = diagnostics.snapshot().session_generation;
    expect(first_generation != 0, "initial session generation");
    expect(!diagnostics.trace_snapshot().enabled,
        "detailed trace is disabled by default");

    diagnostics.record_at(CastPipelineEvent::decoder_request, 41, 100);
    diagnostics.record_at(CastPipelineEvent::decoder_request, 42, 145);
    diagnostics.record_duration(
        CastPipelineEvent::normalize, 42, 150, 175);
    diagnostics.record_duration(
        CastPipelineEvent::normalize, 43, 200, 260);
    diagnostics.increment(CastPipelineCounter::decoded_frames, 2);
    diagnostics.increment(CastPipelineCounter::video_processor_passes);

    auto snapshot = diagnostics.snapshot();
    const auto& requests = snapshot.event(
        CastPipelineEvent::decoder_request);
    expect(requests.count == 2, "request event count");
    expect(requests.last_correlation_id == 42, "request correlation id");
    expect(requests.maximum_gap_qpc == 45, "request maximum gap");
    const auto& normalizes = snapshot.event(CastPipelineEvent::normalize);
    expect(normalizes.count == 2, "duration event count");
    expect(normalizes.total_duration_qpc == 85, "total duration");
    expect(normalizes.maximum_duration_qpc == 60, "maximum duration");
    expect(snapshot.counter(CastPipelineCounter::decoded_frames) == 2,
        "decoded frame counter");

    // Independent transport clients may overlap or reconnect after a long
    // pause. Only gaps within one caller-defined flow are meaningful.
    diagnostics.record_flow_duration(
        CastPipelineEvent::http_send, 1, 0, 300, 310, 101, 188);
    diagnostics.record_flow_duration(
        CastPipelineEvent::http_send, 2, 0, 1300, 1310, 202, 188);
    diagnostics.record_flow_duration(
        CastPipelineEvent::http_send, 3, 1310, 1335, 1345, 202, 188);
    snapshot = diagnostics.snapshot();
    const auto& sends = snapshot.event(CastPipelineEvent::http_send);
    expect(sends.count == 3, "flow event count");
    expect(sends.maximum_gap_qpc == 35,
        "flow gap excludes the unrelated reconnect boundary");
    expect(sends.total_duration_qpc == 30
            && sends.maximum_duration_qpc == 10,
        "flow event durations");

    constexpr std::size_t thread_count = 4;
    constexpr std::size_t increments_per_thread = 5000;
    std::vector<std::thread> workers;
    for (std::size_t i = 0; i < thread_count; ++i) {
        workers.emplace_back([&] {
            for (std::size_t j = 0; j < increments_per_thread; ++j) {
                diagnostics.increment(CastPipelineCounter::gpu_copies);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    snapshot = diagnostics.snapshot();
    expect(snapshot.counter(CastPipelineCounter::gpu_copies)
            == thread_count * increments_per_thread,
        "concurrent counter updates");

    diagnostics.reset();
    snapshot = diagnostics.snapshot();
    expect(snapshot.session_generation == first_generation + 1,
        "reset advances generation");
    expect(snapshot.event(CastPipelineEvent::decoder_request).count == 0,
        "reset clears events");
    expect(snapshot.counter(CastPipelineCounter::gpu_copies) == 0,
        "reset clears counters");

    expect(diagnostics.set_trace_enabled(true), "enable detailed trace");
    diagnostics.record_at(
        CastPipelineEvent::decoder_sample_ready, 44, 300, 7, -3);
    diagnostics.record_duration(
        CastPipelineEvent::overlay_draw, 45, 320, 350, 8, 9);
    std::array<CastPipelineTraceRecord, 4> short_trace{};
    auto trace_count = diagnostics.read_trace(
        short_trace.data(), short_trace.size(), 0);
    expect(trace_count == 2, "read detailed trace");
    expect(short_trace[0].sequence == 1
            && short_trace[0].session_generation == first_generation + 1
            && short_trace[0].qpc == 300
            && short_trace[0].duration_qpc == 0
            && short_trace[0].correlation_id == 44
            && short_trace[0].related_id == 7
            && short_trace[0].value == -3,
        "instant trace fields");
    expect(short_trace[1].sequence == 2
            && short_trace[1].qpc == 350
            && short_trace[1].duration_qpc == 30
            && short_trace[1].related_id == 8
            && short_trace[1].value == 9,
        "duration trace fields");
    expect(diagnostics.read_trace(
            short_trace.data(), short_trace.size(), 2) == 0,
        "trace cursor at latest sequence");

    diagnostics.reset();
    expect(diagnostics.trace_snapshot().latest_sequence == 0,
        "session reset clears detailed trace");
    constexpr auto wrapped_count =
        CastPipelineDiagnostics::kTraceCapacity + 3;
    for (std::size_t index = 0; index < wrapped_count; ++index) {
        diagnostics.record_at(
            CastPipelineEvent::normalize, index, index + 1000);
    }
    std::vector<CastPipelineTraceRecord> wrapped(
        CastPipelineDiagnostics::kTraceCapacity);
    trace_count = diagnostics.read_trace(
        wrapped.data(), static_cast<std::uint32_t>(wrapped.size()), 0);
    const auto trace_state = diagnostics.trace_snapshot();
    expect(trace_count == wrapped.size()
            && trace_state.latest_sequence == wrapped_count
            && trace_state.overwritten_events == 3,
        "bounded trace ring wraps");
    expect(wrapped.front().sequence == 4
            && wrapped.back().sequence == wrapped_count,
        "wrapped trace preserves retained order");

    expect(diagnostics.set_trace_enabled(false), "disable detailed trace");
    diagnostics.record_at(
        CastPipelineEvent::normalize, 999999, 999999);
    expect(diagnostics.trace_snapshot().latest_sequence == wrapped_count,
        "disabled trace preserves existing records");
    return 0;
}

