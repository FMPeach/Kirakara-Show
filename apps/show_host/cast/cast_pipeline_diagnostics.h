#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

// Cast-only, event-driven pipeline telemetry. Recording is allocation-free and
// never logs or performs file I/O. Timestamps use QueryPerformanceCounter on
// Windows (steady_clock ticks in portable tests) and are exposed with their
// frequency so callers can convert gaps without losing precision.
enum class CastPipelineEvent : std::uint8_t {
    decoder_request,
    decoder_sample_ready,
    normalize,
    overlay_draw,
    nv12_compose,
    gpu_copy,
    encoder_process_input,
    encoder_output,
    ts_append,
    http_first_byte,
    http_send,
    underflow,
    recovered,
    count,
};

enum class CastPipelineCounter : std::uint8_t {
    decoded_frames,
    repeated_frames,
    video_processor_passes,
    compute_dispatches,
    gpu_copies,
    explicit_flushes,
    encoder_drops,
    decoder_underflows,
    decoder_recoveries,
    client_rebases,
    texture_creations,
    view_creations,
    missed_output_slots,
    count,
};

// Bit field stored in CastPipelineEvent::encoder_process_input trace values.
// It keeps the public event/statistics ABI stable while making an exact
// program-switch keyframe request and its driver result observable.
namespace cast_encoder_input_diagnostic {
inline constexpr std::int64_t discontinuity = 1LL << 0;
inline constexpr std::int64_t keyframe_requested = 1LL << 1;
inline constexpr std::int64_t keyframe_applied = 1LL << 2;
inline constexpr std::int64_t keyframe_unsupported = 1LL << 3;
inline constexpr std::int64_t keyframe_failed = 1LL << 4;
}  // namespace cast_encoder_input_diagnostic

// CastPipelineEvent::encoder_output stores the encoded access-unit byte count
// in `value`; a negative count identifies a clean-point/keyframe output. Zero
// is never emitted for a valid access unit.
namespace cast_encoder_output_diagnostic {
[[nodiscard]] inline constexpr bool is_keyframe(
        std::int64_t value) noexcept {
    return value < 0;
}

[[nodiscard]] inline constexpr std::uint64_t byte_count(
        std::int64_t value) noexcept {
    return static_cast<std::uint64_t>(value < 0 ? -value : value);
}
}  // namespace cast_encoder_output_diagnostic

struct CastPipelineEventSnapshot {
    std::uint64_t count{};
    std::uint64_t last_correlation_id{};
    std::uint64_t last_qpc{};
    std::uint64_t maximum_gap_qpc{};
    std::uint64_t total_duration_qpc{};
    std::uint64_t maximum_duration_qpc{};
};

struct CastPipelineDiagnosticsSnapshot {
    std::uint64_t session_generation{};
    std::uint64_t qpc_frequency{};
    std::array<CastPipelineEventSnapshot,
        static_cast<std::size_t>(CastPipelineEvent::count)> events{};
    std::array<std::uint64_t,
        static_cast<std::size_t>(CastPipelineCounter::count)> counters{};

    [[nodiscard]] const CastPipelineEventSnapshot& event(
        CastPipelineEvent value) const noexcept;
    [[nodiscard]] std::uint64_t counter(
        CastPipelineCounter value) const noexcept;
};

struct CastPipelineTraceRecord {
    std::uint64_t sequence{};
    std::uint64_t session_generation{};
    std::uint64_t qpc{};
    std::uint64_t duration_qpc{};
    std::uint64_t correlation_id{};
    std::uint64_t related_id{};
    std::int64_t value{};
    CastPipelineEvent event{CastPipelineEvent::decoder_request};
    std::uint32_t thread_id{};
};

struct CastPipelineTraceSnapshot {
    bool enabled{};
    std::uint64_t latest_sequence{};
    std::uint64_t overwritten_events{};
    std::uint64_t dropped_events{};
};

class CastPipelineDiagnostics {
public:
    static constexpr std::size_t kTraceCapacity = 16384;

    CastPipelineDiagnostics() noexcept;

    // Start a new measurement session. Call only while the Cast workers are
    // stopped, before publishing this object to those workers.
    void reset() noexcept;

    // Detailed tracing is opt-in and allocates its fixed ring before becoming
    // visible to producer threads. Writers never wait for a concurrent reader;
    // a contended event is counted as dropped instead.
    [[nodiscard]] bool set_trace_enabled(bool enabled) noexcept;
    [[nodiscard]] CastPipelineTraceSnapshot trace_snapshot() const noexcept;
    [[nodiscard]] std::uint32_t read_trace(
        CastPipelineTraceRecord* output,
        std::uint32_t capacity,
        std::uint64_t after_sequence) const noexcept;

    void record(
        CastPipelineEvent event,
        std::uint64_t correlation_id = 0,
        std::uint64_t related_id = 0,
        std::int64_t value = 0) noexcept;
    void record_at(
        CastPipelineEvent event,
        std::uint64_t correlation_id,
        std::uint64_t qpc,
        std::uint64_t related_id = 0,
        std::int64_t value = 0) noexcept;
    void record_duration(
        CastPipelineEvent event,
        std::uint64_t correlation_id,
        std::uint64_t started_qpc,
        std::uint64_t finished_qpc,
        std::uint64_t related_id = 0,
        std::int64_t value = 0) noexcept;

    // Record one event in a caller-defined flow. The supplied previous QPC is
    // the preceding completion in that same flow; zero starts a new flow and
    // deliberately contributes no gap. This prevents independent HTTP client
    // connections from manufacturing a false transport stall.
    void record_flow_duration(
        CastPipelineEvent event,
        std::uint64_t correlation_id,
        std::uint64_t previous_finished_qpc,
        std::uint64_t started_qpc,
        std::uint64_t finished_qpc,
        std::uint64_t related_id = 0,
        std::int64_t value = 0) noexcept;

    void increment(
        CastPipelineCounter counter,
        std::uint64_t amount = 1) noexcept;

    [[nodiscard]] CastPipelineDiagnosticsSnapshot snapshot() const noexcept;

    [[nodiscard]] static std::uint64_t qpc_now() noexcept;
    [[nodiscard]] static std::uint64_t qpc_frequency() noexcept;

private:
    struct AtomicEvent {
        std::atomic<std::uint64_t> count{};
        std::atomic<std::uint64_t> last_correlation_id{};
        std::atomic<std::uint64_t> last_qpc{};
        std::atomic<std::uint64_t> maximum_gap_qpc{};
        std::atomic<std::uint64_t> total_duration_qpc{};
        std::atomic<std::uint64_t> maximum_duration_qpc{};
    };

    static void update_maximum(
        std::atomic<std::uint64_t>& destination,
        std::uint64_t candidate) noexcept;
    void append_trace(
        CastPipelineEvent event,
        std::uint64_t correlation_id,
        std::uint64_t related_id,
        std::int64_t value,
        std::uint64_t qpc,
        std::uint64_t duration_qpc) noexcept;

    std::atomic<std::uint64_t> session_generation_{};
    std::array<AtomicEvent,
        static_cast<std::size_t>(CastPipelineEvent::count)> events_{};
    std::array<std::atomic<std::uint64_t>,
        static_cast<std::size_t>(CastPipelineCounter::count)> counters_{};
    std::atomic<bool> trace_enabled_{};
    std::atomic<std::uint64_t> trace_dropped_events_{};
    mutable std::mutex trace_mutex_;
    std::unique_ptr<std::array<CastPipelineTraceRecord, kTraceCapacity>>
        trace_records_;
    std::uint64_t trace_latest_sequence_{};
};

