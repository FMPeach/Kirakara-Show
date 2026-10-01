#include "cast_pipeline_diagnostics.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <new>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr std::size_t index_of(CastPipelineEvent value) noexcept {
    return static_cast<std::size_t>(value);
}

constexpr std::size_t index_of(CastPipelineCounter value) noexcept {
    return static_cast<std::size_t>(value);
}

}  // namespace

const CastPipelineEventSnapshot& CastPipelineDiagnosticsSnapshot::event(
        CastPipelineEvent value) const noexcept {
    return events[index_of(value)];
}

std::uint64_t CastPipelineDiagnosticsSnapshot::counter(
        CastPipelineCounter value) const noexcept {
    return counters[index_of(value)];
}

CastPipelineDiagnostics::CastPipelineDiagnostics() noexcept {
    reset();
}

void CastPipelineDiagnostics::reset() noexcept {
    auto generation = session_generation_.load(std::memory_order_relaxed) + 1;
    if (generation == 0) generation = 1;
    for (auto& event : events_) {
        event.count.store(0, std::memory_order_relaxed);
        event.last_correlation_id.store(0, std::memory_order_relaxed);
        event.last_qpc.store(0, std::memory_order_relaxed);
        event.maximum_gap_qpc.store(0, std::memory_order_relaxed);
        event.total_duration_qpc.store(0, std::memory_order_relaxed);
        event.maximum_duration_qpc.store(0, std::memory_order_relaxed);
    }
    for (auto& counter : counters_) {
        counter.store(0, std::memory_order_relaxed);
    }
    trace_dropped_events_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard lock(trace_mutex_);
        trace_latest_sequence_ = 0;
    }
    session_generation_.store(generation, std::memory_order_release);
}

bool CastPipelineDiagnostics::set_trace_enabled(bool enabled) noexcept {
    if (enabled) {
        std::lock_guard lock(trace_mutex_);
        if (!trace_records_) {
            auto records = std::unique_ptr<
                std::array<CastPipelineTraceRecord, kTraceCapacity>>(
                    new (std::nothrow)
                        std::array<CastPipelineTraceRecord, kTraceCapacity>());
            if (!records) return false;
            trace_records_ = std::move(records);
        }
    }
    trace_enabled_.store(enabled, std::memory_order_release);
    return true;
}

CastPipelineTraceSnapshot CastPipelineDiagnostics::trace_snapshot()
        const noexcept {
    CastPipelineTraceSnapshot result;
    result.enabled = trace_enabled_.load(std::memory_order_acquire);
    result.dropped_events = trace_dropped_events_.load(
        std::memory_order_relaxed);
    std::lock_guard lock(trace_mutex_);
    result.latest_sequence = trace_latest_sequence_;
    result.overwritten_events = trace_latest_sequence_ > kTraceCapacity
        ? trace_latest_sequence_ - kTraceCapacity : 0;
    return result;
}

std::uint32_t CastPipelineDiagnostics::read_trace(
        CastPipelineTraceRecord* output,
        std::uint32_t capacity,
        std::uint64_t after_sequence) const noexcept {
    if (!output || capacity == 0
            || after_sequence == std::numeric_limits<std::uint64_t>::max()) {
        return 0;
    }
    std::lock_guard lock(trace_mutex_);
    if (!trace_records_ || after_sequence >= trace_latest_sequence_) return 0;
    const auto oldest = trace_latest_sequence_ >= kTraceCapacity
        ? trace_latest_sequence_ - kTraceCapacity + 1 : 1;
    const auto first = std::max(after_sequence + 1, oldest);
    std::uint32_t copied{};
    for (auto sequence = first;
            sequence <= trace_latest_sequence_ && copied < capacity;
            ++sequence) {
        output[copied++] =
            (*trace_records_)[(sequence - 1) % kTraceCapacity];
    }
    return copied;
}

void CastPipelineDiagnostics::record(
        CastPipelineEvent event,
        std::uint64_t correlation_id,
        std::uint64_t related_id,
        std::int64_t value) noexcept {
    record_at(event, correlation_id, qpc_now(), related_id, value);
}

void CastPipelineDiagnostics::record_at(
        CastPipelineEvent event,
        std::uint64_t correlation_id,
        std::uint64_t qpc,
        std::uint64_t related_id,
        std::int64_t value) noexcept {
    const auto index = index_of(event);
    if (index >= events_.size()) return;
    auto& destination = events_[index];
    const auto previous = destination.last_qpc.exchange(
        qpc, std::memory_order_relaxed);
    if (previous != 0 && qpc >= previous) {
        update_maximum(destination.maximum_gap_qpc, qpc - previous);
    }
    destination.last_correlation_id.store(
        correlation_id, std::memory_order_relaxed);
    destination.count.fetch_add(1, std::memory_order_relaxed);
    append_trace(
        event, correlation_id, related_id, value, qpc, 0);
}

void CastPipelineDiagnostics::record_duration(
        CastPipelineEvent event,
        std::uint64_t correlation_id,
        std::uint64_t started_qpc,
        std::uint64_t finished_qpc,
        std::uint64_t related_id,
        std::int64_t value) noexcept {
    const auto index = index_of(event);
    if (index >= events_.size()) return;
    auto& destination = events_[index];
    const auto previous = destination.last_qpc.exchange(
        finished_qpc, std::memory_order_relaxed);
    if (previous != 0 && finished_qpc >= previous) {
        update_maximum(
            destination.maximum_gap_qpc, finished_qpc - previous);
    }
    destination.last_correlation_id.store(
        correlation_id, std::memory_order_relaxed);
    destination.count.fetch_add(1, std::memory_order_relaxed);

    const auto duration = finished_qpc >= started_qpc
        ? finished_qpc - started_qpc : 0;
    append_trace(event, correlation_id, related_id, value,
        finished_qpc, duration);
    if (finished_qpc < started_qpc) return;
    events_[index].total_duration_qpc.fetch_add(
        duration, std::memory_order_relaxed);
    update_maximum(events_[index].maximum_duration_qpc, duration);
}

void CastPipelineDiagnostics::record_flow_duration(
        CastPipelineEvent event,
        std::uint64_t correlation_id,
        std::uint64_t previous_finished_qpc,
        std::uint64_t started_qpc,
        std::uint64_t finished_qpc,
        std::uint64_t related_id,
        std::int64_t value) noexcept {
    const auto index = index_of(event);
    if (index >= events_.size()) return;
    auto& destination = events_[index];
    destination.last_qpc.store(finished_qpc, std::memory_order_relaxed);
    if (previous_finished_qpc != 0
            && finished_qpc >= previous_finished_qpc) {
        update_maximum(destination.maximum_gap_qpc,
            finished_qpc - previous_finished_qpc);
    }
    destination.last_correlation_id.store(
        correlation_id, std::memory_order_relaxed);
    destination.count.fetch_add(1, std::memory_order_relaxed);

    const auto duration = finished_qpc >= started_qpc
        ? finished_qpc - started_qpc : 0;
    append_trace(event, correlation_id, related_id, value,
        finished_qpc, duration);
    if (finished_qpc < started_qpc) return;
    destination.total_duration_qpc.fetch_add(
        duration, std::memory_order_relaxed);
    update_maximum(destination.maximum_duration_qpc, duration);
}

void CastPipelineDiagnostics::increment(
        CastPipelineCounter counter,
        std::uint64_t amount) noexcept {
    const auto index = index_of(counter);
    if (index >= counters_.size() || amount == 0) return;
    counters_[index].fetch_add(amount, std::memory_order_relaxed);
}

CastPipelineDiagnosticsSnapshot CastPipelineDiagnostics::snapshot()
        const noexcept {
    CastPipelineDiagnosticsSnapshot result;
    result.session_generation = session_generation_.load(
        std::memory_order_acquire);
    result.qpc_frequency = qpc_frequency();
    for (std::size_t index = 0; index < events_.size(); ++index) {
        const auto& source = events_[index];
        auto& destination = result.events[index];
        destination.count = source.count.load(std::memory_order_relaxed);
        destination.last_correlation_id = source.last_correlation_id.load(
            std::memory_order_relaxed);
        destination.last_qpc = source.last_qpc.load(
            std::memory_order_relaxed);
        destination.maximum_gap_qpc = source.maximum_gap_qpc.load(
            std::memory_order_relaxed);
        destination.total_duration_qpc = source.total_duration_qpc.load(
            std::memory_order_relaxed);
        destination.maximum_duration_qpc = source.maximum_duration_qpc.load(
            std::memory_order_relaxed);
    }
    for (std::size_t index = 0; index < counters_.size(); ++index) {
        result.counters[index] = counters_[index].load(
            std::memory_order_relaxed);
    }
    return result;
}

std::uint64_t CastPipelineDiagnostics::qpc_now() noexcept {
#ifdef _WIN32
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value)
        ? static_cast<std::uint64_t>(value.QuadPart) : 0;
#else
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

std::uint64_t CastPipelineDiagnostics::qpc_frequency() noexcept {
#ifdef _WIN32
    LARGE_INTEGER value{};
    return QueryPerformanceFrequency(&value)
        ? static_cast<std::uint64_t>(value.QuadPart) : 0;
#else
    using Period = std::chrono::steady_clock::period;
    return static_cast<std::uint64_t>(Period::den / Period::num);
#endif
}

void CastPipelineDiagnostics::update_maximum(
        std::atomic<std::uint64_t>& destination,
        std::uint64_t candidate) noexcept {
    auto current = destination.load(std::memory_order_relaxed);
    while (candidate > current
            && !destination.compare_exchange_weak(
                current, candidate,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {}
}

void CastPipelineDiagnostics::append_trace(
        CastPipelineEvent event,
        std::uint64_t correlation_id,
        std::uint64_t related_id,
        std::int64_t value,
        std::uint64_t qpc,
        std::uint64_t duration_qpc) noexcept {
    if (!trace_enabled_.load(std::memory_order_acquire)) return;
    std::unique_lock lock(trace_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        trace_dropped_events_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!trace_records_) return;
    const auto sequence = ++trace_latest_sequence_;
    auto& destination =
        (*trace_records_)[(sequence - 1) % kTraceCapacity];
    destination.sequence = sequence;
    destination.session_generation = session_generation_.load(
        std::memory_order_acquire);
    destination.qpc = qpc;
    destination.duration_qpc = duration_qpc;
    destination.correlation_id = correlation_id;
    destination.related_id = related_id;
    destination.value = value;
    destination.event = event;
#ifdef _WIN32
    destination.thread_id = GetCurrentThreadId();
#else
    destination.thread_id = 0;
#endif
}

