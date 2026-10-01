#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Native/Unified Stage-only telemetry. It is opt-in through the process-local
// KIRAKARA_NATIVE_STAGE_DIAGNOSTICS environment variable. The disabled path
// performs one predictable branch per probe and does not read QPC, allocate,
// log or do file I/O. Enabled recording uses fixed-size interval histograms so
// the five-second report can include count, average, approximate P95 and
// maximum. The object is intentionally single-threaded and must stay on the
// Native Stage host thread.
enum class NativeStagePipelineEvent : std::uint8_t {
    stage_tick,
    decoder_request,
    pool_acquire,
    begin_clear,
    video_composite,
    video_cache_copy,
    title_render,
    lyric_render,
    d2d_flush,
    d3d_flush,
    frame_publish,
    physical_present,
    preview_copy,
    preview_flush,
    preview_callback,
    first_frame_gate,
    standby_prime,
    count,
};

enum class NativeStagePipelineCounter : std::uint8_t {
    render_ticks,
    decoder_requests,
    decoder_hits,
    decoder_misses,
    unique_video_frames,
    repeated_video_frames,
    video_cache_refreshes,
    video_cache_reuses,
    published_frames,
    pool_drops,
    repeated_present_frames,
    static_frame_skips,
    d2d_flush_boundaries,
    d3d_flush_boundaries,
    physical_presents,
    preview_publications,
    preview_throttled_frames,
    preview_callbacks,
    missed_render_slots,
    standby_requests,
    standby_decoder_starts,
    standby_ready_frames,
    standby_promotions,
    standby_fallbacks,
    standby_cancellations,
    preview_degraded_to_20,
    preview_degraded_to_15,
    preview_load_recoveries,
    count,
};

struct NativeStagePipelineEventSnapshot {
    std::uint64_t count{};
    std::uint64_t total_duration_qpc{};
    std::uint64_t p95_duration_qpc{};
    std::uint64_t maximum_duration_qpc{};
};

struct NativeStagePipelineDiagnosticsSnapshot {
    std::uint64_t qpc_frequency{};
    std::array<NativeStagePipelineEventSnapshot,
        static_cast<std::size_t>(NativeStagePipelineEvent::count)> events{};
    std::array<std::uint64_t,
        static_cast<std::size_t>(NativeStagePipelineCounter::count)> counters{};

    [[nodiscard]] const NativeStagePipelineEventSnapshot& event(
        NativeStagePipelineEvent value) const noexcept;
    [[nodiscard]] std::uint64_t counter(
        NativeStagePipelineCounter value) const noexcept;
};

class NativeStagePipelineDiagnostics {
public:
    NativeStagePipelineDiagnostics() noexcept;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    // Internal/test control. Product code normally enables telemetry through
    // KIRAKARA_NATIVE_STAGE_DIAGNOSTICS before show_host_create().
    void set_enabled(bool enabled) noexcept;
    void reset_interval() noexcept;

    void record_duration(
        NativeStagePipelineEvent event,
        std::uint64_t started_qpc,
        std::uint64_t finished_qpc) noexcept;
    void record_duration_ticks(
        NativeStagePipelineEvent event,
        std::uint64_t duration_qpc) noexcept;
    void increment(
        NativeStagePipelineCounter counter,
        std::uint64_t amount = 1) noexcept;

    // Called from the Native Stage host thread. At most once per five seconds
    // it publishes an interval report to stderr and OutputDebugString. It
    // never creates a persistent file or changes machine/user configuration.
    void maybe_report() noexcept;

    [[nodiscard]] NativeStagePipelineDiagnosticsSnapshot snapshot()
        const noexcept;

    [[nodiscard]] static std::uint64_t qpc_now() noexcept;
    [[nodiscard]] static std::uint64_t qpc_frequency() noexcept;

private:
    static constexpr std::size_t kHistogramBucketCount = 32;

    struct EventAccumulator {
        std::uint64_t count{};
        std::uint64_t total_duration_qpc{};
        std::uint64_t maximum_duration_qpc{};
        std::array<std::uint64_t, kHistogramBucketCount> histogram{};
    };

    [[nodiscard]] static bool environment_enabled() noexcept;
    [[nodiscard]] std::size_t histogram_bucket(
        std::uint64_t duration_qpc) const noexcept;
    [[nodiscard]] std::uint64_t percentile_95_qpc(
        const EventAccumulator& event) const noexcept;
    void emit_report(std::uint64_t now_qpc) noexcept;

    bool enabled_{};
    std::uint64_t frequency_{};
    std::uint64_t interval_started_qpc_{};
    std::uint64_t next_report_qpc_{};
    std::array<EventAccumulator,
        static_cast<std::size_t>(NativeStagePipelineEvent::count)> events_{};
    std::array<std::uint64_t,
        static_cast<std::size_t>(NativeStagePipelineCounter::count)> counters_{};
};

class NativeStagePipelineDurationScope {
public:
    NativeStagePipelineDurationScope(
        NativeStagePipelineDiagnostics& diagnostics,
        NativeStagePipelineEvent event) noexcept;
    ~NativeStagePipelineDurationScope();

    NativeStagePipelineDurationScope(
        const NativeStagePipelineDurationScope&) = delete;
    NativeStagePipelineDurationScope& operator=(
        const NativeStagePipelineDurationScope&) = delete;

private:
    NativeStagePipelineDiagnostics* diagnostics_{};
    NativeStagePipelineEvent event_{};
    std::uint64_t started_qpc_{};
};
