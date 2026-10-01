#include "native_stage_pipeline_diagnostics.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <limits>

#include <windows.h>

namespace {

constexpr std::size_t index_of(NativeStagePipelineEvent value) noexcept {
    return static_cast<std::size_t>(value);
}

constexpr std::size_t index_of(NativeStagePipelineCounter value) noexcept {
    return static_cast<std::size_t>(value);
}

// Upper bounds in microseconds. The last bucket is an overflow bucket whose
// P95 estimate falls back to the observed maximum.
constexpr std::array<std::uint64_t, 32> kHistogramUpperUs{
    10, 20, 30, 50, 75, 100, 150, 200,
    300, 500, 750, 1000, 1500, 2000, 3000, 4000,
    6000, 8000, 12000, 16000, 20000, 25000, 33000, 50000,
    75000, 100000, 150000, 250000, 500000, 1000000, 2000000,
    std::numeric_limits<std::uint64_t>::max(),
};

constexpr std::array<const char*,
    static_cast<std::size_t>(NativeStagePipelineEvent::count)> kEventNames{
    "tick", "decoder", "pool", "begin_clear", "video", "video_copy",
    "title", "lyrics", "d2d_flush", "d3d_flush", "publish", "present",
    "preview_copy", "preview_flush", "preview_callback", "first_frame",
    "standby_prime",
};

void emit_line(const char* line) noexcept {
    if (!line) return;
    char debugger_line[768]{};
    std::snprintf(debugger_line, std::size(debugger_line), "%s\n", line);
    OutputDebugStringA(debugger_line);
    std::fputs(debugger_line, stderr);
    std::fflush(stderr);
}

double qpc_to_ms(std::uint64_t value, std::uint64_t frequency) noexcept {
    return frequency == 0 ? 0.0
        : static_cast<double>(value) * 1000.0
            / static_cast<double>(frequency);
}

}  // namespace

const NativeStagePipelineEventSnapshot&
NativeStagePipelineDiagnosticsSnapshot::event(
        NativeStagePipelineEvent value) const noexcept {
    return events[index_of(value)];
}

std::uint64_t NativeStagePipelineDiagnosticsSnapshot::counter(
        NativeStagePipelineCounter value) const noexcept {
    return counters[index_of(value)];
}

NativeStagePipelineDiagnostics::NativeStagePipelineDiagnostics() noexcept
    : enabled_(environment_enabled()),
      frequency_(qpc_frequency()) {
    reset_interval();
}

void NativeStagePipelineDiagnostics::set_enabled(bool enabled) noexcept {
    enabled_ = enabled;
    reset_interval();
}

void NativeStagePipelineDiagnostics::reset_interval() noexcept {
    events_ = {};
    counters_ = {};
    interval_started_qpc_ = enabled_ ? qpc_now() : 0;
    next_report_qpc_ = enabled_ && frequency_ != 0
        ? interval_started_qpc_ + frequency_ * 5 : 0;
}

void NativeStagePipelineDiagnostics::record_duration(
        NativeStagePipelineEvent event,
        std::uint64_t started_qpc,
        std::uint64_t finished_qpc) noexcept {
    if (!enabled_ || finished_qpc < started_qpc) return;
    record_duration_ticks(event, finished_qpc - started_qpc);
}

void NativeStagePipelineDiagnostics::record_duration_ticks(
        NativeStagePipelineEvent event,
        std::uint64_t duration_qpc) noexcept {
    if (!enabled_) return;
    const auto index = index_of(event);
    if (index >= events_.size()) return;
    auto& destination = events_[index];
    ++destination.count;
    destination.total_duration_qpc += duration_qpc;
    destination.maximum_duration_qpc = std::max(
        destination.maximum_duration_qpc, duration_qpc);
    ++destination.histogram[histogram_bucket(duration_qpc)];
}

void NativeStagePipelineDiagnostics::increment(
        NativeStagePipelineCounter counter,
        std::uint64_t amount) noexcept {
    if (!enabled_ || amount == 0) return;
    const auto index = index_of(counter);
    if (index >= counters_.size()) return;
    counters_[index] += amount;
}

void NativeStagePipelineDiagnostics::maybe_report() noexcept {
    if (!enabled_ || frequency_ == 0) return;
    const auto now = qpc_now();
    if (next_report_qpc_ == 0) {
        interval_started_qpc_ = now;
        next_report_qpc_ = now + frequency_ * 5;
        return;
    }
    if (now < next_report_qpc_) return;
    emit_report(now);
    reset_interval();
}

NativeStagePipelineDiagnosticsSnapshot
NativeStagePipelineDiagnostics::snapshot() const noexcept {
    NativeStagePipelineDiagnosticsSnapshot result;
    result.qpc_frequency = frequency_;
    for (std::size_t index = 0; index < events_.size(); ++index) {
        const auto& source = events_[index];
        auto& destination = result.events[index];
        destination.count = source.count;
        destination.total_duration_qpc = source.total_duration_qpc;
        destination.p95_duration_qpc = percentile_95_qpc(source);
        destination.maximum_duration_qpc = source.maximum_duration_qpc;
    }
    result.counters = counters_;
    return result;
}

std::uint64_t NativeStagePipelineDiagnostics::qpc_now() noexcept {
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value)
        ? static_cast<std::uint64_t>(value.QuadPart) : 0;
}

std::uint64_t NativeStagePipelineDiagnostics::qpc_frequency() noexcept {
    LARGE_INTEGER value{};
    return QueryPerformanceFrequency(&value)
        ? static_cast<std::uint64_t>(value.QuadPart) : 0;
}

bool NativeStagePipelineDiagnostics::environment_enabled() noexcept {
    wchar_t value[16]{};
    const auto length = GetEnvironmentVariableW(
        L"KIRAKARA_NATIVE_STAGE_DIAGNOSTICS", value,
        static_cast<DWORD>(std::size(value)));
    if (length == 0 || length >= std::size(value)) return false;
    return std::wcscmp(value, L"1") == 0
        || _wcsicmp(value, L"true") == 0
        || _wcsicmp(value, L"on") == 0
        || _wcsicmp(value, L"yes") == 0;
}

std::size_t NativeStagePipelineDiagnostics::histogram_bucket(
        std::uint64_t duration_qpc) const noexcept {
    if (frequency_ == 0) return kHistogramUpperUs.size() - 1;
    const auto duration_us = duration_qpc
            <= std::numeric_limits<std::uint64_t>::max() / 1000000ULL
        ? duration_qpc * 1000000ULL / frequency_
        : std::numeric_limits<std::uint64_t>::max();
    const auto found = std::lower_bound(
        kHistogramUpperUs.begin(), kHistogramUpperUs.end(), duration_us);
    return found == kHistogramUpperUs.end()
        ? kHistogramUpperUs.size() - 1
        : static_cast<std::size_t>(found - kHistogramUpperUs.begin());
}

std::uint64_t NativeStagePipelineDiagnostics::percentile_95_qpc(
        const EventAccumulator& event) const noexcept {
    if (event.count == 0 || frequency_ == 0) return 0;
    const auto target = (event.count * 95 + 99) / 100;
    std::uint64_t accumulated{};
    for (std::size_t index = 0; index < event.histogram.size(); ++index) {
        accumulated += event.histogram[index];
        if (accumulated < target) continue;
        const auto upper_us = kHistogramUpperUs[index];
        if (upper_us == std::numeric_limits<std::uint64_t>::max()) {
            return event.maximum_duration_qpc;
        }
        return upper_us <= std::numeric_limits<std::uint64_t>::max()
                    / frequency_
            ? (upper_us * frequency_ + 999999ULL) / 1000000ULL
            : event.maximum_duration_qpc;
    }
    return event.maximum_duration_qpc;
}

void NativeStagePipelineDiagnostics::emit_report(
        std::uint64_t now_qpc) noexcept {
    const auto interval_qpc = now_qpc >= interval_started_qpc_
        ? now_qpc - interval_started_qpc_ : 0;
    char line[768]{};
    std::snprintf(line, std::size(line),
        "[native-stage %.2fs] ticks=%llu decoder=%llu/%llu miss=%llu "
        "video unique/repeat=%llu/%llu cache refresh/reuse=%llu/%llu "
        "published=%llu pool_drop=%llu "
        "repeat_present=%llu static_skip=%llu present=%llu "
        "interop_flush d2d/d3d=%llu/%llu "
        "preview=%llu skipped=%llu "
        "callbacks=%llu missed_slots=%llu "
        "standby req/start/ready/promote/fallback/cancel="
        "%llu/%llu/%llu/%llu/%llu/%llu "
        "adaptive_preview to20/to15/recover=%llu/%llu/%llu",
        qpc_to_ms(interval_qpc, frequency_) / 1000.0,
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::render_ticks)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::decoder_hits)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::decoder_requests)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::decoder_misses)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::unique_video_frames)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::repeated_video_frames)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::video_cache_refreshes)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::video_cache_reuses)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::published_frames)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::pool_drops)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::repeated_present_frames)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::static_frame_skips)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::physical_presents)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::d2d_flush_boundaries)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::d3d_flush_boundaries)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::preview_publications)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::preview_throttled_frames)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::preview_callbacks)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::missed_render_slots)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::standby_requests)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::standby_decoder_starts)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::standby_ready_frames)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::standby_promotions)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::standby_fallbacks)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::standby_cancellations)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::preview_degraded_to_20)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::preview_degraded_to_15)]),
        static_cast<unsigned long long>(counters_[index_of(
            NativeStagePipelineCounter::preview_load_recoveries)]));
    emit_line(line);

    const auto snapshot_value = snapshot();
    for (std::size_t index = 0; index < snapshot_value.events.size(); ++index) {
        const auto& event = snapshot_value.events[index];
        if (event.count == 0) continue;
        std::snprintf(line, std::size(line),
            "[native-stage] %-16s count=%llu avg=%.3fms p95<=%.3fms "
            "max=%.3fms",
            kEventNames[index],
            static_cast<unsigned long long>(event.count),
            qpc_to_ms(event.total_duration_qpc, frequency_)
                / static_cast<double>(event.count),
            qpc_to_ms(event.p95_duration_qpc, frequency_),
            qpc_to_ms(event.maximum_duration_qpc, frequency_));
        emit_line(line);
    }
}

NativeStagePipelineDurationScope::NativeStagePipelineDurationScope(
        NativeStagePipelineDiagnostics& diagnostics,
        NativeStagePipelineEvent event) noexcept
    : diagnostics_(diagnostics.enabled() ? &diagnostics : nullptr),
      event_(event),
      started_qpc_(diagnostics_
        ? NativeStagePipelineDiagnostics::qpc_now() : 0) {}

NativeStagePipelineDurationScope::~NativeStagePipelineDurationScope() {
    if (!diagnostics_) return;
    diagnostics_->record_duration(
        event_, started_qpc_, NativeStagePipelineDiagnostics::qpc_now());
}
