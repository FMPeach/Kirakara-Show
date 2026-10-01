// Self-serve stutter reproducer driver.
//
// Drives the real show_host DLL through its C API with a single video and
// reports frame cadence, so a 25-50ms render-tick stall (the A/V re-sync
// block) becomes visible without a human watching the screen.
//
//   sync_driver <video_url> [--soak-seconds=N] [--video-master|--audio-master]
//              [--with-audio <vocal> <inst>] [--volume=0]
//
// Defaults: video-master clock, no separate audio (DirectSound not opened),
// 140s soak. With --with-audio the vocal/accompaniment tracks are opened and
// volume is set to 0 so the sync path runs silently.
#include "../apps/show_host/show_host_api.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

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

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void milestone(const char* message) {
    std::cout << "[sync-driver] " << message << std::endl;
}

struct CadenceTracker {
    std::atomic_uint64_t notifications{};
    std::atomic_int64_t last_frame_ns{};
    std::atomic_int64_t maximum_gap_ns{};
    std::atomic_uint64_t gaps_over_16ms{};
    std::atomic_uint64_t gaps_over_20ms{};
    std::atomic_uint64_t gaps_over_25ms{};
    std::atomic_uint64_t gaps_over_33ms{};
    std::atomic_uint64_t gaps_over_40ms{};
    std::atomic_uint64_t gaps_over_50ms{};

    void observe() noexcept {
        notifications.fetch_add(1, std::memory_order_relaxed);
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const auto previous = last_frame_ns.exchange(now, std::memory_order_relaxed);
        if (previous <= 0 || now <= previous) return;
        const auto gap = now - previous;
        auto maximum = maximum_gap_ns.load(std::memory_order_relaxed);
        while (gap > maximum
                && !maximum_gap_ns.compare_exchange_weak(
                    maximum, gap, std::memory_order_relaxed)) {}
        if (gap > 16'000'000) gaps_over_16ms.fetch_add(1, std::memory_order_relaxed);
        if (gap > 20'000'000) gaps_over_20ms.fetch_add(1, std::memory_order_relaxed);
        if (gap > 25'000'000) gaps_over_25ms.fetch_add(1, std::memory_order_relaxed);
        if (gap > 33'000'000) gaps_over_33ms.fetch_add(1, std::memory_order_relaxed);
        if (gap > 40'000'000) gaps_over_40ms.fetch_add(1, std::memory_order_relaxed);
        if (gap > 50'000'000) gaps_over_50ms.fetch_add(1, std::memory_order_relaxed);
    }
};

void report_cadence_to(const CadenceTracker& cadence, const char* label,
        std::ostream& out) {
    out << label
        << " notifications=" << cadence.notifications.load()
        << " max_gap_ms="
        << (cadence.maximum_gap_ns.load() / 1'000'000.0)
        << " >16ms=" << cadence.gaps_over_16ms.load()
        << " >20ms=" << cadence.gaps_over_20ms.load()
        << " >25ms=" << cadence.gaps_over_25ms.load()
        << " >33ms=" << cadence.gaps_over_33ms.load()
        << " >40ms=" << cadence.gaps_over_40ms.load()
        << " >50ms=" << cadence.gaps_over_50ms.load()
        << std::endl;
}

void pump_for(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    MSG message{};
    while (std::chrono::steady_clock::now() < deadline) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        std::this_thread::sleep_for(2ms);
    }
}

std::wstring argument_value(int argc, wchar_t** argv,
        const std::wstring& prefix) {
    for (int i = 2; i < argc; ++i) {
        const std::wstring argument{argv[i]};
        if (argument.rfind(prefix, 0) == 0) {
            return argument.substr(prefix.size());
        }
    }
    return {};
}

bool has_argument(int argc, wchar_t** argv, const std::wstring& value) {
    for (int i = 2; i < argc; ++i) {
        if (std::wstring{argv[i]} == value) return true;
    }
    return false;
}

void report_cadence(const CadenceTracker& cadence, const char* label) {
    report_cadence_to(cadence, label, std::cout);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    expect(argc >= 2, "usage: sync_driver <video_url|@file> [options]");
    auto video = std::wstring{argv[1]};
    if (!video.empty() && video[0] == L'@') {
        std::wifstream file(video.substr(1).c_str());
        std::getline(file, video);
        file.close();
    }
    const auto soak_ms = argument_value(argc, argv, L"--soak-seconds=");
    const auto soak = soak_ms.empty()
        ? 140s
        : std::chrono::seconds{std::wcstoul(soak_ms.c_str(), nullptr, 10)};
    const bool video_master = !has_argument(argc, argv, L"--audio-master");
    const auto lyric = argument_value(argc, argv, L"--lyric=");
    const auto report_path = argument_value(argc, argv, L"--report=");
    const auto vocal = argument_value(argc, argv, L"--with-audio=");
    // --with-audio=<vocal>|<inst>  (pipe separated)
    std::wstring vocal_path;
    std::wstring inst_path;
    if (!vocal.empty()) {
        const auto sep = vocal.find(L'|');
        if (sep != std::wstring::npos) {
            vocal_path = vocal.substr(0, sep);
            inst_path = vocal.substr(sep + 1);
        } else {
            vocal_path = vocal;
        }
    }

    milestone((std::string(video_master ? "video-master" : "audio-master")
        + (vocal_path.empty() ? " no-audio" : " with-audio muted")).c_str());

    auto host = show_host_create();
    expect(host != nullptr, "show_host_create");
    milestone("host created");

    auto texture_source = show_host_create_stage_texture_source(host);
    expect(texture_source != nullptr, "create stage texture source");
    CadenceTracker cadence;
    show_host_stage_texture_source_set_frame_callback(
        texture_source,
        [](void* context, std::uintptr_t, std::uint32_t, std::uint32_t,
                std::uint64_t, std::uint64_t) {
            static_cast<CadenceTracker*>(context)->observe();
        },
        &cadence);
    show_host_stage_texture_source_set_active(texture_source, true);

    auto* device = create_stage_device();
    expect(device != nullptr, "create stage D3D11 device");
    expect(show_host_set_stage_d3d_device(
            host, reinterpret_cast<std::uintptr_t>(device)),
        "set stage D3D11 device");
    milestone("stage device set");

    // Enable the physical stage window (second-screen output) so the composed
    // video + lyric frame is actually visible, like the real app's second
    // display. Without this the render loop only feeds the texture source and
    // nothing appears on screen.
    show_host_set_stage_visible(host, true);
    milestone("physical stage window enabled");

    if (!lyric.empty()) {
        milestone(("with lyric: " + std::string(lyric.begin(), lyric.end())).c_str());
    }
    expect(show_host_load_with_options(
            host, video.c_str(), lyric.c_str(), vocal_path.c_str(), inst_path.c_str(),
            video_master ? SHOW_CLOCK_VIDEO_MASTER : SHOW_CLOCK_AUDIO_MASTER,
            SHOW_TRANSITION_HARD),
        "load video");
    milestone("loaded; playing");
    if (!vocal_path.empty()) {
        show_host_set_volume(host, 0);  // silent
        milestone("audio opened, volume muted to 0");
    }
    show_host_play(host);

    // Simulate the Flutter raster thread: open the publication shared texture
    // on a second D3D11 device and CopyResource it at ~60Hz, creating the same
    // GPU contention the real Flutter presenter produces.
    std::atomic_bool stop_consumer{};
    std::atomic_uint64_t consumed{};
    std::thread gpu_consumer([&] {
        ID3D11Device* cdev = create_stage_device();
        ID3D11DeviceContext* cctx = nullptr;
        if (cdev) cdev->GetImmediateContext(&cctx);
        ID3D11Texture2D* target = nullptr;
        while (!stop_consumer.load()) {
            ShowHostStageTextureFrame frame{};
            if (show_host_stage_texture_source_acquire(
                    texture_source, &frame)) {
                if (cdev && cctx && frame.shared_handle != 0) {
                    ID3D11Texture2D* shared = nullptr;
                    if (SUCCEEDED(cdev->OpenSharedResource(
                            reinterpret_cast<HANDLE>(frame.shared_handle),
                            __uuidof(ID3D11Texture2D),
                            reinterpret_cast<void**>(&shared)))) {
                        if (!target) {
                            D3D11_TEXTURE2D_DESC d{};
                            shared->GetDesc(&d);
                            d.BindFlags = 0;
                            d.MiscFlags = 0;
                            d.Usage = D3D11_USAGE_DEFAULT;
                            if (SUCCEEDED(cdev->CreateTexture2D(
                                    &d, nullptr, &target))) {
                            }
                        }
                        if (target) cctx->CopyResource(target, shared);
                        cctx->Flush();
                        shared->Release();
                    }
                }
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::sleep_for(16ms);
        }
        if (target) target->Release();
        if (cctx) cctx->Release();
        if (cdev) cdev->Release();
    });
    milestone("gpu consumer started");

    const auto started = std::chrono::steady_clock::now();
    auto next_report = started + 10s;
    bool warned_position = false;
    while (std::chrono::steady_clock::now() < started + soak) {
        pump_for(2s);
        const auto state = show_host_get_state(host);
        const auto position = show_host_get_position(host);
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - started).count();
        std::cout << "[sync-driver] t=" << elapsed
                  << "s state=" << state << " pos=" << position
                  << std::endl;
        if (!warned_position && elapsed >= 15 && position <= 0.0) {
            warned_position = true;
            milestone("WARNING: video position not advancing after 15s");
        }
        if (elapsed >= 30 && position <= 0.0) {
            milestone("ABORT: video never started (pos still 0)");
            report_cadence(cadence, "final:");
            show_host_destroy(host);
            return 1;
        }
        if (std::chrono::steady_clock::now() >= next_report) {
            report_cadence(cadence, "mid-soak:");
            next_report = std::chrono::steady_clock::now() + 10s;
        }
        if (state == SHOW_STATE_STOPPED) {
            milestone("stopped early");
            break;
        }
    }
    report_cadence(cadence, "final:");
    if (!report_path.empty()) {
        std::ofstream report(report_path.c_str(), std::ios::app);
        if (report) {
            report << "[sync-driver " << (video_master ? "video-master" : "audio-master")
                   << (lyric.empty() ? " no-lyric" : " with-lyric")
                   << "] ";
            report_cadence_to(cadence, "final:", report);
            report << "[sync-driver] gpu-consumer acquired="
                   << consumed.load() << std::endl;
        }
    }
    stop_consumer.store(true);
    gpu_consumer.join();
    std::cout << "[sync-driver] gpu-consumer acquired="
              << consumed.load() << std::endl;
    milestone("done");
    show_host_destroy(host);
    return 0;
}
