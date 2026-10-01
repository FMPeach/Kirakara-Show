#pragma once

#include <cstdint>

// Registers the bounded, opt-in ETW provider for one NativeStageSession.
// No per-frame work is performed unless an ETW consumer enables the provider.
class NativeStageTraceSession {
public:
    NativeStageTraceSession() noexcept;
    ~NativeStageTraceSession();

    NativeStageTraceSession(const NativeStageTraceSession&) = delete;
    NativeStageTraceSession& operator=(
        const NativeStageTraceSession&) = delete;
};

void trace_native_stage_publish(
    std::uint64_t generation, std::uint64_t frame_id) noexcept;
void trace_native_stage_preview_publication(
    std::uint64_t generation, std::uint64_t frame_id) noexcept;
void trace_native_stage_preview_callback(
    std::uint64_t generation, std::uint64_t frame_id) noexcept;
