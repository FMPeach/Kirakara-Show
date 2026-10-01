#include "../apps/show_host/show_host_api.h"

#include <windows.h>
#include <d3d11.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <iostream>
#include <limits>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void milestone(const char* message) {
    std::cout << "[unified-native-smoke] " << message << std::endl;
}

struct FrameCadenceTracker {
    std::atomic_uint64_t notifications{};
    std::atomic_uint32_t callback_delay_ms{};
    std::atomic_int64_t last_frame_ns{};
    std::atomic_int64_t minimum_gap_ns{};
    std::atomic_int64_t maximum_gap_ns{};
    std::atomic_uint64_t gaps_under_12ms{};
    std::atomic_uint64_t gaps_over_20ms{};
    std::atomic_uint64_t gaps_over_25ms{};
    std::atomic_uint64_t gaps_over_33ms{};

    void reset_cadence() noexcept {
        last_frame_ns.store(0, std::memory_order_relaxed);
        minimum_gap_ns.store(0, std::memory_order_relaxed);
        maximum_gap_ns.store(0, std::memory_order_relaxed);
        gaps_under_12ms.store(0, std::memory_order_relaxed);
        gaps_over_20ms.store(0, std::memory_order_relaxed);
        gaps_over_25ms.store(0, std::memory_order_relaxed);
        gaps_over_33ms.store(0, std::memory_order_relaxed);
    }

    void observe() noexcept {
        notifications.fetch_add(1, std::memory_order_relaxed);
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const auto previous = last_frame_ns.exchange(
            now, std::memory_order_relaxed);
        if (previous <= 0 || now <= previous) return;

        const auto gap = now - previous;
        auto minimum = minimum_gap_ns.load(std::memory_order_relaxed);
        while ((minimum == 0 || gap < minimum)
                && !minimum_gap_ns.compare_exchange_weak(
                    minimum, gap, std::memory_order_relaxed)) {}
        auto maximum = maximum_gap_ns.load(std::memory_order_relaxed);
        while (gap > maximum
                && !maximum_gap_ns.compare_exchange_weak(
                    maximum, gap, std::memory_order_relaxed)) {}
        if (gap < 12'000'000) {
            gaps_under_12ms.fetch_add(1, std::memory_order_relaxed);
        }
        if (gap > 20'000'000) {
            gaps_over_20ms.fetch_add(1, std::memory_order_relaxed);
        }
        if (gap > 25'000'000) {
            gaps_over_25ms.fetch_add(1, std::memory_order_relaxed);
        }
        if (gap > 33'000'000) {
            gaps_over_33ms.fetch_add(1, std::memory_order_relaxed);
        }
        const auto delay = callback_delay_ms.load(std::memory_order_relaxed);
        if (delay != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{delay});
        }
    }
};

void pump_for(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    MSG message{};
    while (std::chrono::steady_clock::now() < deadline) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        std::this_thread::sleep_for(5ms);
    }
}

double sample_notification_rate(
        FrameCadenceTracker& cadence,
        std::chrono::milliseconds duration) {
    const auto before = cadence.notifications.load(std::memory_order_relaxed);
    const auto started = std::chrono::steady_clock::now();
    pump_for(duration);
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    return (cadence.notifications.load(std::memory_order_relaxed) - before)
        / elapsed;
}

struct MonitorSelection {
    RECT primary{};
    RECT secondary{};
    bool has_secondary{};
};

BOOL CALLBACK collect_monitors(
        HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto& selection = *reinterpret_cast<MonitorSelection*>(parameter);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    if ((info.dwFlags & MONITORINFOF_PRIMARY) != 0) {
        selection.primary = info.rcMonitor;
    } else if (!selection.has_secondary) {
        selection.secondary = info.rcMonitor;
        selection.has_secondary = true;
    }
    return TRUE;
}

bool same_rect(const RECT& left, const RECT& right) {
    return left.left == right.left && left.top == right.top
        && left.right == right.right && left.bottom == right.bottom;
}

struct WindowSearch {
    const wchar_t* class_name{};
    HWND result{};
};

BOOL CALLBACK find_window_by_class(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    wchar_t class_name[128]{};
    if (GetClassNameW(window, class_name, 128) > 0
            && wcscmp(class_name, search.class_name) == 0) {
        search.result = window;
        return FALSE;
    }
    return TRUE;
}

HWND find_any_window_by_class(const wchar_t* class_name) {
    WindowSearch search{class_name, nullptr};
    EnumWindows(find_window_by_class, reinterpret_cast<LPARAM>(&search));
    if (search.result) return search.result;
    EnumChildWindows(GetDesktopWindow(), find_window_by_class,
        reinterpret_cast<LPARAM>(&search));
    return search.result;
}

ID3D11Device* create_stage_device() {
    constexpr D3D_FEATURE_LEVEL levels[]{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    ID3D11Device* device{};
    auto result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT
            | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &device,
        nullptr,
        nullptr);
    if (result == E_INVALIDARG) {
        result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT
                | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            levels + 1,
            1,
            D3D11_SDK_VERSION,
            &device,
            nullptr,
            nullptr);
    }
    return SUCCEEDED(result) ? device : nullptr;
}

std::chrono::seconds parse_soak_duration(int argc, wchar_t** argv) {
    constexpr std::wstring_view prefix = L"--soak-seconds=";
    for (int index = 9; index < argc; ++index) {
        const std::wstring_view argument{argv[index]};
        if (!argument.starts_with(prefix)) continue;
        const auto value = argument.substr(prefix.size());
        if (value.empty()) return {};
        wchar_t* end{};
        const auto seconds = std::wcstoul(value.data(), &end, 10);
        if (end == value.data() || *end != L'\0') return {};
        return std::chrono::seconds{seconds};
    }
    return {};
}

bool has_argument(int argc, wchar_t** argv, std::wstring_view expected) {
    for (int index = 9; index < argc; ++index) {
        if (std::wstring_view{argv[index]} == expected) return true;
    }
    return false;
}

std::uint64_t file_time_ticks(const FILETIME& value) {
    ULARGE_INTEGER ticks{};
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    return ticks.QuadPart;
}

struct ProcessSnapshot {
    std::uint64_t cpu_ticks{};
    std::size_t working_set{};
    std::size_t private_bytes{};
    DWORD handles{};
    DWORD gdi_objects{};
    DWORD user_objects{};
};

ProcessSnapshot process_snapshot() {
    const auto process = GetCurrentProcess();
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    expect(GetProcessTimes(process, &created, &exited, &kernel, &user),
        "read process CPU time");
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    expect(GetProcessMemoryInfo(process,
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
            sizeof(memory)),
        "read process memory counters");
    DWORD handles{};
    expect(GetProcessHandleCount(process, &handles),
        "read process handle count");
    return ProcessSnapshot{
        file_time_ticks(kernel) + file_time_ticks(user),
        memory.WorkingSetSize,
        memory.PrivateUsage,
        handles,
        GetGuiResources(process, GR_GDIOBJECTS),
        GetGuiResources(process, GR_USEROBJECTS),
    };
}

void run_stage_soak(ShowHostHandle host,
        ShowHostStageTextureSource texture_source,
        FrameCadenceTracker& cadence,
        std::chrono::seconds duration,
        bool physical_output,
        bool allow_transient_stage_fps) {
    if (duration.count() <= 0) return;

    ShowHostStageFrameStats started_stats{};
    started_stats.struct_size = sizeof(started_stats);
    expect(show_host_get_stage_frame_stats(host, &started_stats),
        "read Stage statistics before soak");
    const auto started_process = process_snapshot();
    const auto started_notifications =
        cadence.notifications.load(std::memory_order_relaxed);
    cadence.reset_cadence();
    const auto started_at = std::chrono::steady_clock::now();
    auto sample_started_at = started_at;
    auto sample_stats = started_stats;
    auto last_frame_changed_at = started_at;
    std::atomic_bool stop_texture_consumer{};
    std::atomic_uint64_t consumed_frame_changes{};
    // A nonzero step lets the soak fail fast if the simulated Flutter raster
    // consumer ever blocks while acquiring the stable publication texture.
    std::atomic_int texture_consumer_step{};
    std::thread texture_consumer([&] {
        std::uint64_t generation{};
        std::uint64_t frame_id{};
        while (!stop_texture_consumer.load(std::memory_order_relaxed)) {
            texture_consumer_step.store(1, std::memory_order_relaxed);
            ShowHostStageTextureFrame frame{};
            if (show_host_stage_texture_source_acquire(
                    texture_source, &frame)) {
                texture_consumer_step.store(2, std::memory_order_relaxed);
                if (frame.generation != generation || frame.frame_id != frame_id) {
                    generation = frame.generation;
                    frame_id = frame.frame_id;
                    consumed_frame_changes.fetch_add(1,
                        std::memory_order_relaxed);
                }
            }
            texture_consumer_step.store(5, std::memory_order_relaxed);
            std::this_thread::sleep_for(16ms);
        }
        texture_consumer_step.store(6, std::memory_order_relaxed);
    });
    auto observed_frame_changes =
        consumed_frame_changes.load(std::memory_order_relaxed);
    double minimum_produced_fps = std::numeric_limits<double>::max();
    double minimum_presented_fps = std::numeric_limits<double>::max();
    double latest_produced_fps{};
    double latest_presented_fps{};
    std::uint64_t replay_count{};

    std::cout << "Unified Stage soak started for " << duration.count()
              << " seconds" << (physical_output ? " with physical output"
                                                  : " with texture output")
              << std::endl;

    const auto deadline = started_at + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        pump_for(100ms);
        const auto now = std::chrono::steady_clock::now();

        const auto frame_changes =
            consumed_frame_changes.load(std::memory_order_relaxed);
        if (frame_changes != observed_frame_changes) {
            observed_frame_changes = frame_changes;
            last_frame_changed_at = now;
        }

        expect(now - last_frame_changed_at < 1500ms,
            "Unified Stage stopped publishing for 1.5 seconds during soak");

        const auto media_duration = show_host_get_duration(host);
        const auto position = show_host_get_position(host);
        if (media_duration > 1.0 && position >= media_duration - 0.5) {
            show_host_seek(host, 0.0);
            show_host_play(host);
            ++replay_count;
        }

        if (now - sample_started_at < 5s) continue;
        ShowHostStageFrameStats current{};
        current.struct_size = sizeof(current);
        expect(show_host_get_stage_frame_stats(host, &current),
            "read periodic Stage statistics during soak");
        const auto elapsed = std::chrono::duration<double>(
            now - sample_started_at).count();
        const auto produced_fps =
            (current.produced_frames - sample_stats.produced_frames) / elapsed;
        const auto presented_fps =
            (current.presented_frames - sample_stats.presented_frames) / elapsed;
        minimum_produced_fps = std::min(minimum_produced_fps, produced_fps);
        latest_produced_fps = produced_fps;
        if (physical_output) {
            minimum_presented_fps = std::min(
                minimum_presented_fps, presented_fps);
            latest_presented_fps = presented_fps;
        }
        std::cout << "  soak "
                  << std::chrono::duration_cast<std::chrono::seconds>(
                         now - started_at).count()
                  << "s: produced=" << produced_fps << " fps";
        if (physical_output) {
            std::cout << ", presented=" << presented_fps << " fps";
        }
        std::cout << ", dropped="
                  << current.dropped_frames - sample_stats.dropped_frames
                  << std::endl;
        sample_started_at = now;
        sample_stats = current;
    }

    stop_texture_consumer.store(true, std::memory_order_relaxed);
    const auto consumer_stop_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (texture_consumer_step.load(std::memory_order_relaxed) != 6
            && std::chrono::steady_clock::now() < consumer_stop_deadline) {
        std::this_thread::sleep_for(10ms);
    }
    if (texture_consumer_step.load(std::memory_order_relaxed) != 6) {
        std::cerr << "FAIL: Flutter texture consumer did not stop; step="
                  << texture_consumer_step.load(std::memory_order_relaxed)
                  << std::endl;
        std::_Exit(EXIT_FAILURE);
    }
    texture_consumer.join();

    ShowHostStageFrameStats finished_stats{};
    finished_stats.struct_size = sizeof(finished_stats);
    expect(show_host_get_stage_frame_stats(host, &finished_stats),
        "read Stage statistics after soak");
    const auto finished_process = process_snapshot();
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started_at).count();
    const auto produced_fps =
        (finished_stats.produced_frames - started_stats.produced_frames)
        / elapsed;
    const auto presented_fps =
        (finished_stats.presented_frames - started_stats.presented_frames)
        / elapsed;
    if (minimum_produced_fps == std::numeric_limits<double>::max()) {
        minimum_produced_fps = produced_fps;
        latest_produced_fps = produced_fps;
    }
    if (physical_output
            && minimum_presented_fps == std::numeric_limits<double>::max()) {
        minimum_presented_fps = presented_fps;
        latest_presented_fps = presented_fps;
    }
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto process_seconds = static_cast<double>(
        finished_process.cpu_ticks - started_process.cpu_ticks) / 10000000.0;
    const auto one_core_cpu = process_seconds / elapsed * 100.0;
    const auto machine_cpu = one_core_cpu /
        static_cast<double>(std::max<DWORD>(1, system.dwNumberOfProcessors));
    const auto mib_delta = [](std::size_t after, std::size_t before) {
        return static_cast<double>(after) / (1024.0 * 1024.0)
            - static_cast<double>(before) / (1024.0 * 1024.0);
    };

    std::cout << "Unified Stage soak result: produced=" << produced_fps
              << " fps, minimum 5s window=" << minimum_produced_fps
              << " fps";
    if (physical_output) {
        std::cout << ", presented=" << presented_fps
                  << " fps, minimum 5s window=" << minimum_presented_fps
                  << " fps";
    }
    std::cout << ", CPU=" << one_core_cpu << "% of one core / "
              << machine_cpu << "% of machine, working-set delta="
              << mib_delta(finished_process.working_set,
                     started_process.working_set)
              << " MiB, private delta="
              << mib_delta(finished_process.private_bytes,
                     started_process.private_bytes)
              << " MiB, handle delta="
              << static_cast<long long>(finished_process.handles)
                     - static_cast<long long>(started_process.handles)
              << ", GDI delta="
              << static_cast<long long>(finished_process.gdi_objects)
                     - static_cast<long long>(started_process.gdi_objects)
              << ", USER delta="
              << static_cast<long long>(finished_process.user_objects)
                     - static_cast<long long>(started_process.user_objects)
              << ", replay count=" << replay_count
              << ", max publish gap="
              << static_cast<double>(cadence.maximum_gap_ns.load(
                     std::memory_order_relaxed)) / 1'000'000.0
              << " ms, min publish gap="
              << static_cast<double>(cadence.minimum_gap_ns.load(
                     std::memory_order_relaxed)) / 1'000'000.0
              << " ms, gaps <12ms="
              << cadence.gaps_under_12ms.load(std::memory_order_relaxed)
              << ", gaps >20/25/33ms="
              << cadence.gaps_over_20ms.load(std::memory_order_relaxed)
              << '/'
              << cadence.gaps_over_25ms.load(std::memory_order_relaxed)
              << '/'
              << cadence.gaps_over_33ms.load(std::memory_order_relaxed)
              << std::endl;

    if (!IsDebuggerPresent()) {
        expect(produced_fps >= 50.0,
            "Unified Stage soak production fell below 50 fps");
        if (allow_transient_stage_fps) {
            expect(latest_produced_fps >= 50.0,
                "Unified Stage did not recover 50 fps after hotplug");
        } else {
            expect(minimum_produced_fps >= 45.0,
                "Unified Stage soak had a five-second window below 45 fps");
        }
    }
    if (physical_output && !IsDebuggerPresent()) {
        expect(presented_fps >= 50.0,
            "physical Stage soak presentation fell below 50 fps");
        if (allow_transient_stage_fps) {
            expect(latest_presented_fps >= 50.0,
                "physical Stage did not recover 50 fps after hotplug");
        } else {
            expect(minimum_presented_fps >= 45.0,
                "physical Stage soak had a five-second window below 45 fps");
        }
    }
    expect(cadence.notifications.load(std::memory_order_relaxed)
            > started_notifications,
        "Flutter texture notifications stopped during Stage soak");
}

void exercise_adaptive_preview_policy(
        ShowHostHandle host,
        FrameCadenceTracker& cadence,
        HWND stage,
        HWND surface) {
    milestone("adaptive Preview pressure test started");
    show_host_seek(host, 0.0);
    show_host_play(host);
    pump_for(500ms);

    // The real Flutter callback is intentionally cheap. A bounded delay here
    // emulates a saturated publication consumer without adding a production
    // test hook or changing the C ABI.
    cadence.callback_delay_ms.store(25, std::memory_order_relaxed);
    pump_for(4500ms);
    const auto degraded_fps = sample_notification_rate(cadence, 1500ms);
    cadence.callback_delay_ms.store(0, std::memory_order_relaxed);
    std::cout << "Adaptive Preview pressured sample: " << degraded_fps
              << " fps" << std::endl;
    expect(degraded_fps >= 8.0 && degraded_fps <= 22.0,
        "sustained Native deadline misses did not lower Preview cadence");

    show_host_set_stage_visible(host, false);
    pump_for(400ms);
    expect(!IsWindowVisible(stage) && !IsWindowVisible(surface),
        "adaptive Preview test could not release physical Stage");
    show_host_set_stage_visible(host, true);
    pump_for(600ms);
    expect(IsWindowVisible(stage) && IsWindowVisible(surface),
        "adaptive Preview test could not restore physical Stage");
    const auto reset_fps = sample_notification_rate(cadence, 1500ms);
    std::cout << "Adaptive Preview lease-reset sample: " << reset_fps
              << " fps" << std::endl;
    expect(reset_fps >= 22.0 && reset_fps <= 40.0,
        "new physical Stage lease did not reset Preview to 30 fps");
    milestone("adaptive Preview pressure test completed");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    expect(argc >= 9,
        "usage: native-smoke video1 krl1 vocal1 inst1 "
        "video2 krl2 vocal2 inst2 [--soak-seconds=N] "
        "[--allow-transient-stage-fps] [--texture-only] "
        "[--force-primary-stage] [--exercise-adaptive-preview] "
        "[--stage-diagnostics]");
    const auto soak_duration = parse_soak_duration(argc, argv);
    const auto allow_transient_stage_fps = has_argument(
        argc, argv, L"--allow-transient-stage-fps");
    const auto texture_only = has_argument(argc, argv, L"--texture-only");
    const auto force_primary_stage = has_argument(
        argc, argv, L"--force-primary-stage");
    const auto exercise_adaptive_preview = has_argument(
        argc, argv, L"--exercise-adaptive-preview");
    const auto stage_diagnostics = has_argument(
        argc, argv, L"--stage-diagnostics");
    if (stage_diagnostics) {
        SetEnvironmentVariableW(
            L"KIRAKARA_NATIVE_STAGE_DIAGNOSTICS", L"1");
    }

    MonitorSelection monitors;
    EnumDisplayMonitors(nullptr, nullptr, collect_monitors,
        reinterpret_cast<LPARAM>(&monitors));
    const bool exercise_physical_stage =
        !texture_only && (monitors.has_secondary || force_primary_stage);
    expect(!exercise_adaptive_preview || exercise_physical_stage,
        "adaptive Preview exercise requires physical Stage output");
    const auto physical_stage_rect = force_primary_stage
        ? monitors.primary : monitors.secondary;
    auto host = show_host_create();
    expect(host != nullptr, "ShowHost should be created");
    milestone("host created");
    ShowHostStageOverlayState overlay{};
    overlay.struct_size = sizeof(overlay);
    overlay.revision = 12;
    overlay.qr_decoration.struct_size = sizeof(overlay.qr_decoration);
    overlay.announcement_decoration.struct_size =
        sizeof(overlay.announcement_decoration);
    expect(show_host_set_stage_overlay_state(host, &overlay),
        "disabled Stage overlay state should cross the host ABI");
    expect(show_host_get_stage_overlay_revision(host) == 12,
        "ShowHost should publish the accepted overlay revision");
    auto texture_source = show_host_create_stage_texture_source(host);
    expect(texture_source != nullptr,
        "Flutter Stage texture source should be created");
    FrameCadenceTracker texture_cadence;
    show_host_stage_texture_source_set_frame_callback(
        texture_source,
        [](void* context, std::uintptr_t, std::uint32_t, std::uint32_t,
                std::uint64_t, std::uint64_t) {
            static_cast<FrameCadenceTracker*>(context)->observe();
        },
        &texture_cadence);
    show_host_stage_texture_source_set_active(texture_source, true);

    expect(show_host_load_with_options(host,
            argv[1], argv[2], argv[3], argv[4],
            SHOW_CLOCK_AUDIO_MASTER, SHOW_TRANSITION_HARD),
        "cold first song should load");
    milestone("first song loaded");
    show_host_play(host);
    pump_for(4500ms);
    milestone("first song playback sampled");
    expect(show_host_get_state(host) == SHOW_STATE_PLAYING,
        "first song should be playing");
    expect(show_host_get_position(host) > 0.5,
        "first song clock should advance");
    const auto position_before_inactive_cast_stop =
        show_host_get_position(host);
    show_host_stop_cast_stream(host);
    pump_for(250ms);
    expect(show_host_get_state(host) == SHOW_STATE_PLAYING,
        "inactive Cast stop should preserve native playback state");
    expect(show_host_get_position(host)
            >= position_before_inactive_cast_stop,
        "inactive Cast stop should not rewind native playback");
    expect(texture_cadence.notifications.load(std::memory_order_relaxed) > 0,
        "Flutter texture source receives frame notifications");
    ShowHostStageTextureFrame texture_frame{};
    expect(show_host_stage_texture_source_acquire(
            texture_source, &texture_frame),
        "Flutter texture source acquires the latest Stage frame");
    expect(texture_frame.shared_handle != 0
            && texture_frame.lease_token == 0,
        "Flutter texture frame exposes a stable publication handle");
    expect(texture_frame.width == 1920 && texture_frame.height == 1080,
        "controller texture profile remains fixed at 1080P");
    pump_for(500ms);
    ShowHostStageTextureFrame stable_texture_frame{};
    expect(show_host_stage_texture_source_acquire(
            texture_source, &stable_texture_frame),
        "Flutter texture source reacquires its publication texture");
    expect(stable_texture_frame.shared_handle == texture_frame.shared_handle,
        "controller publication handle stays stable between Stage frames");
    expect(stable_texture_frame.generation != texture_frame.generation
            || stable_texture_frame.frame_id > texture_frame.frame_id,
        "stable publication texture advances its frame metadata");

    // Flutter can recreate its renderer with a new D3D11 device instance on
    // the same adapter. The Host must rebuild the publication texture and
    // expose its new shared handle without permitting cross-adapter reads.
    auto* replacement_device = create_stage_device();
    expect(replacement_device != nullptr,
        "replacement Stage D3D11 device should be created");
    const auto notifications_before_device_handoff =
        texture_cadence.notifications.load(std::memory_order_relaxed);
    expect(show_host_set_stage_d3d_device(
            host,
            reinterpret_cast<std::uintptr_t>(replacement_device)),
        "Stage should accept a replacement D3D11 device");
    milestone("replacement D3D11 device accepted");
    replacement_device->Release();
    pump_for(1500ms);
    expect(texture_cadence.notifications.load(std::memory_order_relaxed)
            > notifications_before_device_handoff,
        "Stage should resume publishing after D3D11 device handoff");
    ShowHostStageTextureFrame replacement_texture_frame{};
    expect(show_host_stage_texture_source_acquire(
            texture_source, &replacement_texture_frame),
        "Flutter texture source should recover after device handoff");
    expect(replacement_texture_frame.shared_handle != 0
            && replacement_texture_frame.shared_handle
                != texture_frame.shared_handle,
        "device handoff should publish a new shared texture");
    show_host_release_stage_texture_frame(
        replacement_texture_frame.lease_token);
    milestone("replacement texture acquired");

    pump_for(500ms);

    const auto stage = find_any_window_by_class(
        L"KirakaraShowHostStage");
    expect(stage != nullptr, "Stage HWND should exist");
    const auto surface = FindWindowExW(
        stage, nullptr, L"KirakaraShowHostVideoBlank", nullptr);
    expect(surface != nullptr, "Unified swap-chain child should exist");
    expect(!IsWindowVisible(stage) && !IsWindowVisible(surface),
        "Flutter texture should exclusively own the controller preview");
    expect(find_any_window_by_class(L"KirakaraShowHostSubtitle") == nullptr,
        "legacy subtitle HWND class must not exist");
    const auto media_clock = find_any_window_by_class(
        L"KirakaraShowHostMediaClock");
    expect(media_clock != nullptr && IsWindowVisible(media_clock),
        "MediaEngine clock HWND should remain alive offscreen");
    RECT media_clock_rect{};
    GetWindowRect(media_clock, &media_clock_rect);
    expect(media_clock_rect.right < monitors.primary.left,
        "MediaEngine clock HWND must not participate in visible Stage output");

    if (exercise_physical_stage) {
        const auto width = static_cast<std::uint32_t>(
            physical_stage_rect.right - physical_stage_rect.left);
        const auto height = static_cast<std::uint32_t>(
            physical_stage_rect.bottom - physical_stage_rect.top);
        expect(width != 0 && height != 0,
            "physical Stage target monitor should have pixel bounds");
        show_host_set_stage_window_rect(host,
            physical_stage_rect.left,
            physical_stage_rect.top,
            width,
            height);
        show_host_set_stage_visible(host, true);
        pump_for(1500ms);
        milestone("physical Stage enabled");
        RECT stage_rect{};
        GetWindowRect(stage, &stage_rect);
        expect(GetParent(stage) == nullptr,
            "physical Stage should be a top-level HWND");
        expect(same_rect(stage_rect, physical_stage_rect),
            "physical Stage should use monitor pixel bounds");
        expect(IsWindowVisible(surface),
            "physical Stage should keep the Unified surface visible");
        ShowHostStageTextureFrame physical_texture_frame{};
        expect(show_host_stage_texture_source_acquire(
                texture_source, &physical_texture_frame),
            "Flutter texture source survives physical profile replacement");
        expect(physical_texture_frame.width == width
                && physical_texture_frame.height == height,
            "Flutter texture source follows the active physical profile");
        show_host_release_stage_texture_frame(
            physical_texture_frame.lease_token);

        SendMessageW(stage, WM_DISPLAYCHANGE, 32,
            MAKELPARAM(width, height));
        pump_for(1000ms);
        milestone("display change recovered");
        ShowHostStageTextureFrame display_change_frame{};
        expect(show_host_stage_texture_source_acquire(
                texture_source, &display_change_frame),
            "Flutter texture source recovers after display mode change");
        expect(display_change_frame.shared_handle != 0,
            "display recovery publishes a new shared texture");
        show_host_release_stage_texture_frame(
            display_change_frame.lease_token);

        ShowHostStageFrameStats before_stats{};
        before_stats.struct_size = sizeof(before_stats);
        expect(show_host_get_stage_frame_stats(host, &before_stats),
            "read Stage frame statistics before sampling");
        const auto preview_notifications_before =
            texture_cadence.notifications.load(std::memory_order_relaxed);
        const auto frame_sample_started = std::chrono::steady_clock::now();
        pump_for(2000ms);
        const auto frame_sample_elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - frame_sample_started).count();
        ShowHostStageFrameStats after_stats{};
        after_stats.struct_size = sizeof(after_stats);
        expect(show_host_get_stage_frame_stats(host, &after_stats),
            "read Stage frame statistics after sampling");
        const auto produced_fps =
            (after_stats.produced_frames - before_stats.produced_frames)
            / frame_sample_elapsed;
        const auto presented_fps =
            (after_stats.presented_frames - before_stats.presented_frames)
            / frame_sample_elapsed;
        const auto preview_fps =
            (texture_cadence.notifications.load(std::memory_order_relaxed)
                - preview_notifications_before)
            / frame_sample_elapsed;
        const auto dropped =
            after_stats.dropped_frames - before_stats.dropped_frames;
        std::cout << "Unified Stage dual-screen sample: produced="
                  << produced_fps << " fps, presented=" << presented_fps
                  << " fps, preview=" << preview_fps
                  << " fps, dropped=" << dropped << std::endl;
        if (!IsDebuggerPresent()) {
            expect(produced_fps >= 50.0,
                "dual-screen Stage production should stay near 60 fps");
            expect(presented_fps >= 50.0,
                "physical Stage presentation should stay near 60 fps");
            expect(preview_fps >= 50.0,
                "dual-screen native Preview should follow Stage near 60 fps");
        }
        if (exercise_adaptive_preview) {
            exercise_adaptive_preview_policy(
                host, texture_cadence, stage, surface);
        }
    }

    milestone("retired publication generation replaced");

    run_stage_soak(host, texture_source, texture_cadence,
        soak_duration, exercise_physical_stage, allow_transient_stage_fps);
    milestone("soak completed");

    const auto before_pause = show_host_get_position(host);
    show_host_pause(host);
    pump_for(300ms);
    expect(show_host_get_state(host) == SHOW_STATE_PAUSED,
        "native Stage should enter paused state");
    ShowHostStageFrameStats paused_start_stats{};
    paused_start_stats.struct_size = sizeof(paused_start_stats);
    expect(show_host_get_stage_frame_stats(host, &paused_start_stats),
        "read Stage statistics after pause settles");
    const auto paused_notifications =
        texture_cadence.notifications.load(std::memory_order_relaxed);
    pump_for(900ms);
    ShowHostStageFrameStats paused_end_stats{};
    paused_end_stats.struct_size = sizeof(paused_end_stats);
    expect(show_host_get_stage_frame_stats(host, &paused_end_stats),
        "read Stage statistics after static pause");
    const auto after_pause = show_host_get_position(host);
    expect(std::abs(after_pause - before_pause) < 0.25,
        "paused native Stage clock should remain stable");
    expect(paused_end_stats.produced_frames
            - paused_start_stats.produced_frames <= 3,
        "static pause should not keep composing Stage frames");
    expect(paused_end_stats.presented_frames
            - paused_start_stats.presented_frames <= 3,
        "static pause should not keep presenting physical Stage frames");
    expect(texture_cadence.notifications.load(std::memory_order_relaxed)
            - paused_notifications <= 3,
        "static pause should not keep notifying Flutter Preview");

    expect(show_host_load_with_options(host,
            argv[5], argv[6], argv[7], argv[8],
            SHOW_CLOCK_AUDIO_MASTER, SHOW_TRANSITION_SEAMLESS),
        "unprepared second song should load");
    milestone("second song loaded");
    show_host_play(host);
    milestone("second song play requested");
    pump_for(4500ms);
    milestone("second song playback sampled");
    expect(show_host_get_state(host) == SHOW_STATE_PLAYING,
        "second song should be playing");
    expect(show_host_get_position(host) > 0.5,
        "second song clock should advance");
    expect(exercise_physical_stage
            ? IsWindowVisible(surface)
            : !IsWindowVisible(surface),
        "song switching should preserve the selected Stage sink");
    expect(find_any_window_by_class(L"KirakaraShowHostSubtitle") == nullptr,
        "song switching must not create a legacy subtitle layer");

    expect(show_host_prepare_next(host,
            argv[1], argv[2], argv[3], argv[4],
            SHOW_CLOCK_AUDIO_MASTER),
        "first song should be accepted for bounded preparation");
    milestone("first song preparation requested");
    // Preparation is deliberately asynchronous and has no public readiness
    // poll. The repository smoke fixtures are local and short, so this window
    // covers KRL/audio setup plus the Native standby decoder's first frame.
    pump_for(2500ms);
    const auto notifications_before_prepared_switch =
        texture_cadence.notifications.load(std::memory_order_relaxed);
    expect(show_host_load_with_options(host,
            argv[1], argv[2], argv[3], argv[4],
            SHOW_CLOCK_AUDIO_MASTER, SHOW_TRANSITION_SEAMLESS),
        "prepared first song should load");
    milestone("prepared first song loaded");
    show_host_play(host);
    pump_for(2500ms);
    expect(show_host_get_state(host) == SHOW_STATE_PLAYING,
        "prepared song should be playing");
    expect(show_host_get_position(host) > 0.5,
        "prepared song clock should start at zero and advance");
    expect(texture_cadence.notifications.load(std::memory_order_relaxed)
            > notifications_before_prepared_switch,
        "prepared promotion should publish a new complete Stage frame");
    milestone("prepared song promotion sampled");

    if (exercise_physical_stage) {
        milestone("disabling physical Stage");
        show_host_set_stage_visible(host, false);
        milestone("physical Stage disable accepted");
        pump_for(500ms);
        expect(!IsWindowVisible(stage) && !IsWindowVisible(surface),
            "disabling physical output should restore texture-only preview");
        ShowHostStageTextureFrame restored_texture_frame{};
        expect(show_host_stage_texture_source_acquire(
                texture_source, &restored_texture_frame),
            "Flutter texture source survives physical Stage shutdown");
        expect(restored_texture_frame.width == 1920
                && restored_texture_frame.height == 1080,
            "controller texture returns to the fixed 1080P profile");
        show_host_release_stage_texture_frame(
            restored_texture_frame.lease_token);
    }

    milestone("destroying host");
    show_host_destroy(host);
    milestone("host destroyed");
    texture_frame = {};
    expect(!show_host_stage_texture_source_acquire(
            texture_source, &texture_frame),
        "texture source becomes inert after ShowHost destruction");
    show_host_destroy_stage_texture_source(texture_source);
    std::cout << "Unified ShowHost native Stage smoke passed"
              << (exercise_physical_stage ? " with physical HWND output\n"
                                           : " with texture output only\n");
    return 0;
}
