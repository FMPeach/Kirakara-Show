#include "native_stage_trace.h"

#include <windows.h>
#include <evntprov.h>

#include <atomic>
#include <cwchar>
#include <iterator>
#include <mutex>

namespace {

constexpr ULONGLONG kFrameKeyword = 0x1;
constexpr UCHAR kVerboseLevel = 5;
constexpr GUID kProviderGuid{
    0x9b2572f2, 0x7f04, 0x4059,
    {0x9e, 0x9c, 0xfc, 0xba, 0xfb, 0xf5, 0xb0, 0xde}};
std::mutex g_provider_mutex;
std::uint32_t g_provider_users{};
std::atomic<REGHANDLE> g_provider_handle{};

std::uint64_t qpc_now() noexcept {
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value)
        ? static_cast<std::uint64_t>(value.QuadPart) : 0;
}

void write_frame_event(
        const wchar_t* name,
        std::uint64_t generation,
        std::uint64_t frame_id) noexcept {
    const auto handle = g_provider_handle.load(std::memory_order_acquire);
    if (handle == 0 || !EventProviderEnabled(
            handle, kVerboseLevel, kFrameKeyword)) {
        return;
    }
    wchar_t payload[192]{};
    const auto length = std::swprintf(
        payload, std::size(payload),
        L"%ls qpc=%llu generation=%llu frame_id=%llu thread_id=%lu",
        name,
        static_cast<unsigned long long>(qpc_now()),
        static_cast<unsigned long long>(generation),
        static_cast<unsigned long long>(frame_id),
        static_cast<unsigned long>(GetCurrentThreadId()));
    if (length <= 0 || static_cast<std::size_t>(length) >= std::size(payload)) {
        return;
    }
    EventWriteString(
        handle, kVerboseLevel, kFrameKeyword, payload);
}

}  // namespace

NativeStageTraceSession::NativeStageTraceSession() noexcept {
    std::lock_guard lock(g_provider_mutex);
    if (g_provider_users++ == 0) {
        REGHANDLE handle{};
        if (EventRegister(&kProviderGuid, nullptr, nullptr, &handle)
                == ERROR_SUCCESS) {
            g_provider_handle.store(handle, std::memory_order_release);
        }
    }
}

NativeStageTraceSession::~NativeStageTraceSession() {
    std::lock_guard lock(g_provider_mutex);
    if (g_provider_users == 0 || --g_provider_users != 0) return;
    const auto handle = g_provider_handle.exchange(
        0, std::memory_order_acq_rel);
    if (handle != 0) EventUnregister(handle);
}

void trace_native_stage_publish(
        std::uint64_t generation, std::uint64_t frame_id) noexcept {
    write_frame_event(L"StagePublish", generation, frame_id);
}

void trace_native_stage_preview_publication(
        std::uint64_t generation, std::uint64_t frame_id) noexcept {
    write_frame_event(L"PreviewPublication", generation, frame_id);
}

void trace_native_stage_preview_callback(
        std::uint64_t generation, std::uint64_t frame_id) noexcept {
    write_frame_event(L"PreviewCallback", generation, frame_id);
}
