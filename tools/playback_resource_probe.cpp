// Compare full host playback with identical media/settings across DLL builds.
#include "../apps/show_host/show_host_api.h"
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

struct GpuCounters {
    PDH_HQUERY query{};
    PDH_HCOUNTER engines{}, dedicated{}, shared{};
    std::wstring prefix = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
    bool available{};
    GpuCounters() {
        available = PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS
            && PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engines) == ERROR_SUCCESS;
        if (query) {
            PdhAddEnglishCounterW(query, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &dedicated);
            PdhAddEnglishCounterW(query, L"\\GPU Process Memory(*)\\Shared Usage", 0, &shared);
            PdhCollectQueryData(query);
        }
    }
    ~GpuCounters() { if (query) PdhCloseQuery(query); }
    double sum(PDH_HCOUNTER counter, const wchar_t* kind = nullptr) {
        if (!counter) return 0.0;
        DWORD bytes{}, count{};
        const auto format = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100;
        if (PdhGetFormattedCounterArrayW(counter, format, &bytes, &count, nullptr) != PDH_MORE_DATA) return 0.0;
        std::vector<unsigned char> buffer(bytes);
        auto* values = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(counter, format, &bytes, &count, values) != ERROR_SUCCESS) return 0.0;
        double total{};
        for (DWORD index = 0; index < count; ++index) {
            const std::wstring_view name(values[index].szName);
            if (!name.starts_with(prefix) || (kind && name.find(kind) == name.npos)) continue;
            const auto& value = values[index].FmtValue;
            if ((value.CStatus == PDH_CSTATUS_VALID_DATA || value.CStatus == PDH_CSTATUS_NEW_DATA)
                    && std::isfinite(value.doubleValue)) total += value.doubleValue;
        }
        return total;
    }
};

double process_cpu_seconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const auto integer = [](const FILETIME& time) {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return (integer(kernel) + integer(user)) / 10000000.0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 5) { std::cerr << "probe video external_audio output_json sample_seconds\n"; return 2; }
    const int seconds = std::max(5, _wtoi(argv[4]));
    const auto host = show_host_create();
    if (!host) return 3;
    std::atomic<std::uint64_t> notifications{};
    const auto source = show_host_create_stage_texture_source(host);
    show_host_stage_texture_source_set_frame_callback(source,
        [](void* opaque, std::uintptr_t, std::uint32_t, std::uint32_t, std::uint64_t, std::uint64_t) {
            static_cast<std::atomic<std::uint64_t>*>(opaque)->fetch_add(1, std::memory_order_relaxed);
        }, &notifications);
    show_host_stage_texture_source_set_active(source, true);
    const auto load_started = std::chrono::steady_clock::now();
    const auto audio_path = std::wstring_view(argv[2]) == L"-" ? L"" : argv[2];
    if (!show_host_load_with_clock(host, argv[1], L"", audio_path, L"", SHOW_CLOCK_VIDEO_MASTER)) return 4;
    const auto load_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - load_started).count();
    show_host_set_volume(host, 0);
    show_host_play(host);
    // First-frame opening, predecode, and driver allocations are warm-up.
    std::this_thread::sleep_for(std::chrono::seconds(4));
    if (show_host_get_state(host) != SHOW_STATE_PLAYING || show_host_get_position(host) < 1.0
            || notifications.load() < 30) {
        std::cerr << "playback did not reach a stable rendering state\n"; return 5;
    }
    GpuCounters gpu;
    ShowHostStageFrameStats before{};
    before.struct_size = sizeof(before);
    show_host_get_stage_frame_stats(host, &before);
    const auto first_notification = notifications.load();
    const auto position_start = show_host_get_position(host);
    const auto cpu_start = process_cpu_seconds();
    const auto measured_started = std::chrono::steady_clock::now();
    double working{}, private_bytes{}, video_decode{}, gpu_3d{}, video_processing{}, copy{}, dedicated{}, shared{};
    std::uint64_t peak_working{};
    int samples{};
    for (int index = 0; index < seconds * 2; ++index) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (gpu.query) PdhCollectQueryData(gpu.query);
        video_decode += gpu.sum(gpu.engines, L"engtype_VideoDecode");
        gpu_3d += gpu.sum(gpu.engines, L"engtype_3D");
        video_processing += gpu.sum(gpu.engines, L"engtype_VideoProcessing");
        copy += gpu.sum(gpu.engines, L"engtype_Copy");
        dedicated += gpu.sum(gpu.dedicated);
        shared += gpu.sum(gpu.shared);
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) return 6;
        working += static_cast<double>(memory.WorkingSetSize);
        private_bytes += static_cast<double>(memory.PrivateUsage);
        peak_working = std::max(peak_working, static_cast<std::uint64_t>(memory.WorkingSetSize));
        ++samples;
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - measured_started).count();
    const auto cpu_seconds = process_cpu_seconds() - cpu_start;
    ShowHostStageFrameStats after{};
    after.struct_size = sizeof(after);
    show_host_get_stage_frame_stats(host, &after);
    const auto position_end = show_host_get_position(host);
    const auto final_notification = notifications.load();
    const auto state = show_host_get_state(host);
    ShowHostStageTextureFrame publication{};
    const auto texture_ok = show_host_stage_texture_source_acquire(source, &publication);
    if (publication.lease_token) show_host_release_stage_texture_frame(publication.lease_token);
    show_host_pause(host);
    show_host_stage_texture_source_set_active(source, false);
    show_host_destroy_stage_texture_source(source);
    show_host_destroy(host);
    const auto mb = 1024.0 * 1024.0;
    std::ofstream result{std::filesystem::path(argv[3])};
    result << "{\n"
        << "\"cpu_percent_machine\":" << cpu_seconds / elapsed * 100.0 / GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) << ",\n"
        << "\"cpu_core_equivalents\":" << cpu_seconds / elapsed << ",\n"
        << "\"working_set_mb\":" << working / samples / mb << ",\n"
        << "\"peak_working_set_mb\":" << peak_working / mb << ",\n"
        << "\"private_bytes_mb\":" << private_bytes / samples / mb << ",\n"
        << "\"gpu_counters_available\":" << (gpu.available ? "true" : "false") << ",\n"
        << "\"gpu_video_decode_percent\":" << video_decode / samples << ",\n"
        << "\"gpu_3d_percent\":" << gpu_3d / samples << ",\n"
        << "\"gpu_video_processing_percent\":" << video_processing / samples << ",\n"
        << "\"gpu_copy_percent\":" << copy / samples << ",\n"
        << "\"gpu_dedicated_mb\":" << dedicated / samples / mb << ",\n"
        << "\"gpu_shared_mb\":" << shared / samples / mb << ",\n"
        << "\"stage_fps\":" << (after.produced_frames - before.produced_frames) / elapsed << ",\n"
        << "\"preview_fps\":" << (final_notification - first_notification) / elapsed << ",\n"
        << "\"stage_dropped_frames\":" << after.dropped_frames - before.dropped_frames << ",\n"
        << "\"position_start\":" << position_start << ",\n"
        << "\"position_end\":" << position_end << ",\n"
        << "\"load_ms\":" << load_ms << ",\n"
        << "\"elapsed_seconds\":" << elapsed << ",\n"
        << "\"texture_valid\":" << (texture_ok ? "true" : "false") << ",\n"
        << "\"output_width\":" << publication.width << ",\n"
        << "\"output_height\":" << publication.height << ",\n"
        << "\"playing_at_end\":" << (state == SHOW_STATE_PLAYING ? "true" : "false") << "\n}\n";
    return texture_ok && state == SHOW_STATE_PLAYING ? 0 : 7;
}
