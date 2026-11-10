#include "../apps/show_host/media/idle_background.h"
#include "../apps/show_host/cast/cast_stage_renderer.h"
#include "../apps/show_host/show_host_api.h"
#include "kirakara/show/win32/offscreen_surface.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
using namespace kirakara::show::win32;

namespace {

void expect(bool value, const char* message) {
    if (value) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

std::vector<std::uint8_t> read_bgra(ID3D11Texture2D* texture) {
    if (!texture) return {};
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    const auto width = desc.Width;
    const auto height = desc.Height;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> staging;
    texture->GetDevice(&device);
    device->GetImmediateContext(&context);
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return {};
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return {};
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4U);
    for (UINT y = 0; y < height; ++y) {
        std::copy_n(static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch,
            width * 4U, pixels.data() + static_cast<std::size_t>(y) * width * 4U);
    }
    context->Unmap(staging.Get(), 0);
    return pixels;
}

bool close_pixels(const std::vector<std::uint8_t>& a,
        const std::vector<std::uint8_t>& b) {
    if (a.empty() || a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::abs(static_cast<int>(a[i]) - b[i]) > 1) return false;
    }
    return true;
}

void test_pixels(const std::wstring& path) {
    for (const auto [width, height] : {
            std::pair{1920U, 1080U}, {1280U, 720U}, {640U, 360U},
            {960U, 540U}, {1024U, 576U}, {768U, 576U}, {641U, 361U}}) {
        std::vector<std::uint8_t> bgra;
        expect(load_idle_background_bgra(path, width, height, bgra),
            "decode/scale external background");
        expect(bgra.size() == static_cast<std::size_t>(width) * height * 4U,
            "BGRA dimensions");
        for (std::size_t i = 3; i < bgra.size(); i += 4) {
            expect(bgra[i] == 255, "idle background remains opaque");
        }
        std::vector<std::uint8_t> nv12;
        if ((width & 1U) || (height & 1U)) {
            expect(!load_idle_background_nv12(path, width, height, nv12),
                "NV12 rejects odd dimensions without changing native support");
            continue;
        }
        expect(load_idle_background_nv12(path, width, height, nv12),
            "create one-time NV12 background");
        const auto area = static_cast<std::size_t>(width) * height;
        expect(nv12.size() == area * 3U / 2U, "NV12 plane dimensions");
        for (std::size_t i = 0; i < area; ++i) {
            const auto* p = bgra.data() + i * 4U;
            const auto y = std::lround(16.0F
                + (46.5594F * p[2] + 156.6288F * p[1] + 15.8118F * p[0]) / 255.0F);
            expect(nv12[i] == y, "BT.709 luma agrees with BGRA image");
        }
        for (std::uint32_t y = 0; y < height; y += 2) {
            for (std::uint32_t x = 0; x < width; x += 2) {
                double r{}, g{}, b{};
                for (auto dy : {0U, 1U}) for (auto dx : {0U, 1U}) {
                    const auto* p = bgra.data()
                        + (static_cast<std::size_t>(y + dy) * width + x + dx) * 4U;
                    b += p[0]; g += p[1]; r += p[2];
                }
                const auto index = area + static_cast<std::size_t>(y / 2) * width + x;
                const auto u = std::lround(128.0
                    + (-25.664 * r - 86.336 * g + 112.0 * b) / 1020.0);
                const auto v = std::lround(128.0
                    + (112.0 * r - 101.730 * g - 10.270 * b) / 1020.0);
                expect(std::abs(static_cast<int>(nv12[index]) - u) <= 1
                        && std::abs(static_cast<int>(nv12[index + 1]) - v) <= 1,
                    "4:2:0 chroma agrees with averaged BGRA pixels");
            }
        }
    }
    std::vector<std::uint8_t> rejected{1};
    expect(!load_idle_background_bgra(L"", 320, 180, rejected)
            && rejected.empty(), "empty source disables the image");
    expect(!load_idle_background_bgra(path, 0, 180, rejected), "reject zero output size");
}

void test_cast_cache(const std::wstring& path) {
    CastStageRenderer renderer;
    expect(renderer.configure({640, 360, 60, 1}), "configure Cast C output");
    auto source = std::make_shared<IdleBackgroundSource>(IdleBackgroundSource{path, 1});
    renderer.set_idle_background_source(source);
    expect(renderer.compose_idle_frame() && !renderer.idle_frame_reused(),
        "first idle frame draws image");
    renderer.finalize_frame();
    auto* texture = static_cast<ID3D11Texture2D*>(
        renderer.compositor().overlay_surface().native_texture());
    std::vector<std::uint8_t> expected;
    expect(load_idle_background_bgra(path, 640, 360, expected), "decode Cast reference");
    expect(close_pixels(read_bgra(texture), expected), "Cast C displays the image");
    for (int i = 0; i < 100; ++i) {
        expect(renderer.compose_idle_frame() && renderer.idle_frame_reused(),
            "unchanged idle frame reuses composition");
    }
    expect(renderer.compose_idle_frame(false) && !renderer.idle_frame_reused(),
        "active audio-only source uses its existing plain base");
    renderer.finalize_frame();
    expect(!close_pixels(read_bgra(texture), expected), "plain base is not the idle artwork");
    expect(renderer.compose_idle_frame() && !renderer.idle_frame_reused(),
        "returning idle restores image once");
    renderer.finalize_frame();
    renderer.invalidate_idle_frame(); // Simulate drawing title/lyrics on this target.
    expect(renderer.compose_idle_frame() && !renderer.idle_frame_reused(),
        "overlay draw invalidates static idle composition");
    renderer.finalize_frame();
    expect(close_pixels(read_bgra(texture), expected), "idle restore leaves no overlay residue");

    auto* device = static_cast<ID3D11Device*>(renderer.native_d3d_device());
    IdleBackgroundTexture cache;
    expect(cache.configure(device, 640, 360, *source), "create immutable still texture");
    auto* first = cache.texture();
    expect(cache.configure(device, 640, 360, *source) && cache.texture() == first,
        "same source/output keeps GPU texture identity");
    expect(cache.configure(device, 641, 361, *source), "native texture accepts odd dimensions");
    auto missing = std::make_shared<IdleBackgroundSource>(
        IdleBackgroundSource{path + L".missing", 2});
    expect(!cache.configure(device, 640, 360, *missing) && !cache.texture(),
        "missing image retires stale pixels");
    renderer.set_idle_background_source(missing);
    expect(renderer.compose_idle_frame(), "missing optional image keeps a usable plain frame");
    renderer.finalize_frame();
    expect(renderer.compose_idle_frame() && renderer.idle_frame_reused(),
        "failed image load is not retried each frame");
}

ShowHostStageTextureFrame wait_for_frame(ShowHostStageTextureSource source,
        ID3D11Device* device, const std::vector<std::uint8_t>& expected,
        std::uint32_t width, std::uint32_t height) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        ShowHostStageTextureFrame frame{};
        if (show_host_stage_texture_source_acquire(source, &frame)
                && frame.width == width && frame.height == height) {
            ComPtr<ID3D11Texture2D> texture;
            if (SUCCEEDED(device->OpenSharedResource(
                    reinterpret_cast<HANDLE>(frame.shared_handle),
                    IID_PPV_ARGS(&texture)))
                    && close_pixels(read_bgra(texture.Get()), expected)) {
                show_host_release_stage_texture_frame(frame.lease_token);
                return frame;
            }
            show_host_release_stage_texture_frame(frame.lease_token);
        }
        std::this_thread::sleep_for(10ms);
    }
    expect(false, "Host idle frame reaches both preview topologies");
    return {};
}

void test_host(const std::wstring& path) {
    D3D11TextureSurface consumer;
    expect(consumer.resize(16, 9), "create preview consumer device");
    auto* device = static_cast<ID3D11Device*>(consumer.native_d3d_device());
    auto host = show_host_create();
    expect(host != nullptr, "create Host");
    expect(!show_host_set_idle_background_image(nullptr, path.c_str()), "reject null Host");
    expect(!show_host_set_idle_background_image(host, L"relative.png"), "reject relative path");
    expect(show_host_set_idle_background_image(host, path.c_str()), "set UTF-16 image path");
    auto source = show_host_create_stage_texture_source(host);
    expect(source != nullptr, "create clean preview sink");
    show_host_stage_texture_source_set_active(source, true);
    std::vector<std::uint8_t> expected;
    expect(load_idle_background_bgra(path, 1920, 1080, expected), "decode preview reference");
    const auto initial = wait_for_frame(source, device, expected, 1920, 1080);
    expect(show_host_get_state(host) == SHOW_STATE_IDLE
            && show_host_get_position(host) == 0.0
            && show_host_get_active_video_decoder_owners(host) == 0
            && !show_host_is_buffering(host),
        "still image leaves clocks, buffering and decoder ownership alone");

    // The Preview now samples the same program as a physical Stage HWND.
    show_host_set_stage_window_rect(host, -32000, -32000, 320, 180);
    show_host_set_stage_visible(host, true);
    expect(load_idle_background_bgra(path, 320, 180, expected), "decode physical reference");
    wait_for_frame(source, device, expected, 320, 180);
    show_host_set_stage_visible(host, false);
    expect(load_idle_background_bgra(path, 1920, 1080, expected), "restore preview reference");
    const auto restored = wait_for_frame(source, device, expected, 1920, 1080);
    expect(restored.generation == initial.generation, "output changes preserve program generation");

    expect(show_host_set_idle_background_image(host, nullptr), "clear configured image");
    expected.assign(1920U * 1080U * 4U, 0);
    for (std::size_t i = 3; i < expected.size(); i += 4) expected[i] = 255;
    const auto cleared = wait_for_frame(source, device, expected, 1920, 1080);
    expect(cleared.generation == restored.generation, "image update preserves timeline generation");
    show_host_destroy_stage_texture_source(source);
    show_host_destroy(host);
}

void test_cast_transport(const std::wstring& path) {
    auto host = show_host_create();
    expect(host && show_host_set_idle_background_image(host, path.c_str()),
        "configure Cast's external idle image");
    expect(show_host_start_cast_stream(host, 0), "start real idle Cast transport");
    std::this_thread::sleep_for(1500ms);
    ShowHostCastCapabilityReport report{};
    report.struct_size = sizeof(report);
    expect(show_host_get_cast_capability_report(host, &report), "read selected Cast path");
    ShowHostCastPipelineStats first{};
    first.struct_size = sizeof(first);
    expect(show_host_get_cast_pipeline_stats(host, &first), "read warm idle counters");
    std::this_thread::sleep_for(750ms);
    ShowHostCastPipelineStats last{};
    last.struct_size = sizeof(last);
    expect(show_host_get_cast_pipeline_stats(host, &last), "read stable idle counters");
    expect(last.video_processor_passes == first.video_processor_passes
            && last.compute_dispatches == first.compute_dispatches
            && last.gpu_copies == first.gpu_copies
            && last.texture_creations == first.texture_creations
            && last.view_creations == first.view_creations,
        "idle transport reuses GPU resources without per-frame VP/copy/dispatch");
    expect(last.events[SHOW_CAST_EVENT_ENCODER_PROCESS_INPUT].count
                > first.events[SHOW_CAST_EVENT_ENCODER_PROCESS_INPUT].count
            && last.events[SHOW_CAST_EVENT_ENCODER_OUTPUT].count
                > first.events[SHOW_CAST_EVENT_ENCODER_OUTPUT].count,
        "static picture continues producing live encoded frames");
    expect(show_host_get_state(host) == SHOW_STATE_IDLE
            && show_host_get_position(host) == 0.0
            && show_host_get_active_video_decoder_owners(host) == 0
            && !show_host_is_buffering(host),
        "Cast still keeps media clocks and decoder ownership idle");
    expect(show_host_set_idle_background_image(host, nullptr), "clear image while casting");
    std::this_thread::sleep_for(250ms);
    expect(show_host_set_idle_background_image(host, path.c_str()), "restore image while casting");
    std::this_thread::sleep_for(500ms);
    expect(show_host_get_cast_stream_port(host) != 0, "image update preserves Cast session");
    show_host_stop_cast_stream(host);
    show_host_destroy(host);
    std::cout << "[idle-background] live Cast backend=" << report.backend
        << ", idle VP=" << last.video_processor_passes
        << ", compute=" << last.compute_dispatches
        << ", copies=" << last.gpu_copies << std::endl;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    expect(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "initialize COM");
    const auto directory = std::filesystem::temp_directory_path()
        / (L"Kirakara-idle-\u7a7a\u95f2 " + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto fixture = directory / L"background \u56fe\u7247.png";
    {
        OffscreenSurface image;
        expect(image.resize(16, 9), "create fixture bitmap");
        image.begin_draw();
        image.clear({0.25F, 0.5F, 0.75F, 0.5F});
        expect(image.end_draw() && image.save_png(fixture.wstring()), "write fixture PNG");
    }
    const auto path = argc > 1 ? std::wstring(argv[1]) : fixture.wstring();
    std::cout << "[idle-background] BGRA/NV12 scaling" << std::endl;
    test_pixels(path);
    std::cout << "[idle-background] immutable textures and Cast C cache" << std::endl;
    test_cast_cache(path);
    std::cout << "[idle-background] Host FFI, Preview and physical Stage" << std::endl;
    test_host(path);
    if (argc > 2 && std::wstring_view(argv[2]) == L"--cast") {
        test_cast_transport(path);
    }
    std::filesystem::remove(fixture);
    std::filesystem::remove(directory);
    CoUninitialize();
    std::cout << "Idle background: BGRA/NV12, cached Cast C, Native Preview/Stage and FFI passed.\n";
    return EXIT_SUCCESS;
}
