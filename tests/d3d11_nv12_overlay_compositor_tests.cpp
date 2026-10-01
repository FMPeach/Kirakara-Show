#include "../apps/show_host/cast/cast_nv12_surface_pool.h"
#include "../apps/show_host/cast/cast_pipeline_diagnostics.h"
#include "../apps/show_host/cast/d3d11_cast_capabilities.h"
#include "../apps/show_host/cast/d3d11_nv12_overlay_compositor.h"

#include "kirakara/show/win32/d3d11_texture_surface.hpp"

#include <windows.h>
#include <d3d10.h>
#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

using kirakara::show::Color;
using kirakara::show::win32::D3D11TextureSurface;

namespace {

constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 32;

template <typename T>
void release(T*& value) noexcept {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void expect_near(
        std::uint8_t actual,
        int expected,
        int tolerance,
        std::string_view message) {
    if (std::abs(static_cast<int>(actual) - expected) > tolerance) {
        std::cerr << "FAIL: " << message << " (actual="
                  << static_cast<int>(actual) << ", expected="
                  << expected << ")\n";
        std::exit(EXIT_FAILURE);
    }
}

bool create_device(ID3D11Device** device, ID3D11DeviceContext** context) {
    constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT
        | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL requested[]{D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    auto result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
        nullptr, flags, requested, static_cast<UINT>(std::size(requested)),
        D3D11_SDK_VERSION, device, &level, context);
    if (SUCCEEDED(result)) return true;
    return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
        nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested,
        static_cast<UINT>(std::size(requested)), D3D11_SDK_VERSION,
        device, &level, context));
}

void fill_nv12(
        ID3D11DeviceContext* context,
        ID3D11Texture2D* texture,
        std::uint8_t y,
        std::uint8_t u,
        std::uint8_t v) {
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(kWidth) * kHeight * 3U / 2U, y);
    auto* uv = pixels.data() + static_cast<std::size_t>(kWidth) * kHeight;
    for (std::size_t index = 0;
            index < static_cast<std::size_t>(kWidth) * kHeight / 2U;
            index += 2) {
        uv[index] = u;
        uv[index + 1] = v;
    }
    context->UpdateSubresource(
        texture, 0, nullptr, pixels.data(), kWidth,
        static_cast<UINT>(pixels.size()));
}

struct Nv12Pixels {
    std::vector<std::uint8_t> y;
    std::vector<std::uint8_t> uv;
};

Nv12Pixels read_nv12(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    auto staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    ID3D11Texture2D* staging{};
    expect(SUCCEEDED(device->CreateTexture2D(
        &staging_desc, nullptr, &staging)), "create NV12 staging texture");
    context->CopyResource(staging, texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    expect(SUCCEEDED(context->Map(
        staging, 0, D3D11_MAP_READ, 0, &mapped)), "map NV12 output");
    Nv12Pixels result;
    result.y.resize(static_cast<std::size_t>(desc.Width) * desc.Height);
    result.uv.resize(static_cast<std::size_t>(desc.Width) * desc.Height / 2U);
    const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData);
    for (std::uint32_t row = 0; row < desc.Height; ++row) {
        std::copy_n(bytes + static_cast<std::size_t>(row) * mapped.RowPitch,
            desc.Width,
            result.y.data() + static_cast<std::size_t>(row) * desc.Width);
    }
    const auto* uv = bytes
        + static_cast<std::size_t>(mapped.RowPitch) * desc.Height;
    for (std::uint32_t row = 0; row < desc.Height / 2U; ++row) {
        std::copy_n(uv + static_cast<std::size_t>(row) * mapped.RowPitch,
            desc.Width,
            result.uv.data() + static_cast<std::size_t>(row) * desc.Width);
    }
    context->Unmap(staging, 0);
    release(staging);
    return result;
}

std::uint8_t luma(const Nv12Pixels& pixels, std::uint32_t x,
        std::uint32_t y) {
    return pixels.y[static_cast<std::size_t>(y) * kWidth + x];
}

std::uint8_t chroma_u(const Nv12Pixels& pixels, std::uint32_t x,
        std::uint32_t y) {
    return pixels.uv[static_cast<std::size_t>(y) * kWidth + x * 2U];
}

std::uint8_t chroma_v(const Nv12Pixels& pixels, std::uint32_t x,
        std::uint32_t y) {
    return pixels.uv[static_cast<std::size_t>(y) * kWidth + x * 2U + 1U];
}

void clear_overlay(D3D11TextureSurface& surface) {
    surface.begin_draw();
    surface.clear(Color{0.0F, 0.0F, 0.0F, 0.0F});
    expect(surface.end_draw(), "clear BGRA overlay");
}

void fill_overlay_rect(
        D3D11TextureSurface& surface,
        const CastOverlayRect& rect,
        Color color) {
    surface.begin_draw();
    const auto filled = surface.clear_rect(
        static_cast<float>(rect.x), static_cast<float>(rect.y),
        static_cast<float>(rect.x + rect.width),
        static_cast<float>(rect.y + rect.height), color);
    const auto ended = surface.end_draw();
    expect(filled && ended, "fill BGRA overlay rect");
    surface.flush_d2d();
}

CastOverlayFrame overlay_frame(
        D3D11TextureSurface& surface,
        CastOverlayRect rect,
        std::uint64_t version,
        bool empty = false) {
    CastOverlayFrame frame;
    frame.texture = surface.native_texture();
    frame.width = kWidth;
    frame.height = kHeight;
    frame.dirty_rect = rect;
    frame.coverage_rect = rect;
    frame.content_version = version;
    frame.empty = empty;
    frame.changed = true;
    return frame;
}

}  // namespace

int main() {
    expect(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)),
        "COM initialization");
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    expect(create_device(&device, &context), "D3D11 device creation");
    ID3D10Multithread* multithread{};
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    D3D11CastCapabilityProbeOptions probe_options;
    probe_options.width = kWidth;
    probe_options.height = kHeight;
    probe_options.frame_rate_num = 60;
    probe_options.frame_rate_den = 1;
    probe_options.probe_hardware_encoder = false;
    const auto capability = D3D11CastCapabilityProbe::run(
        device, probe_options);
    if (!capability.nv12_plane_srvs.passed()
            || !capability.nv12_plane_uavs.passed()
            || (!capability.uav_video_encoder_bind.passed()
                && !capability.compose_to_encoder_copy.passed())) {
        std::cout << "SKIP: device has no complete NV12 plane-view path\n";
        release(multithread);
        release(context);
        release(device);
        CoUninitialize();
        return EXIT_SUCCESS;
    }

    CastPipelineDiagnostics diagnostics;
    CastNv12SurfacePool base_pool;
    CastNv12SurfacePoolConfig base_config;
    base_config.width = kWidth;
    base_config.height = kHeight;
    base_config.capacity = 2;
    base_config.create_plane_srvs = true;
    base_config.create_plane_uavs = false;
    base_config.video_processor_output = false;
    base_config.encoder_compatible = false;
    expect(base_pool.configure(device, base_config, &diagnostics),
        "configure source NV12 pool");
    auto base_lease = base_pool.try_acquire();
    expect(static_cast<bool>(base_lease), "acquire source NV12 texture");
    auto* base_texture = static_cast<ID3D11Texture2D*>(
        base_lease.compose_texture());
    fill_nv12(context, base_texture, 64, 128, 128);

    D3D11TextureSurface overlay_surface;
    expect(overlay_surface.resize_on_device(device, kWidth, kHeight),
        "create BGRA overlay texture");
    clear_overlay(overlay_surface);
    overlay_surface.flush_d2d();

    D3D11Nv12OverlayCompositor compositor;
    D3D11Nv12OverlayCompositorConfig compositor_config;
    compositor_config.width = kWidth;
    compositor_config.height = kHeight;
    compositor_config.surface_count = 4;
    compositor_config.separate_encoder_texture =
        !capability.uav_video_encoder_bind.passed();
    expect(compositor.configure(device, compositor_config, &diagnostics),
        "configure NV12 overlay compositor");
    expect(compositor.prepare_overlay_texture(
        overlay_surface.native_texture()), "prepare stable overlay SRV");

    D3D11Nv12OverlayInput input;
    input.texture = base_texture;
    input.y_srv = base_lease.y_srv();
    input.uv_srv = base_lease.uv_srv();
    input.retention = base_lease.retention_token();
    input.frame_id = 17;
    input.generation = 3;

    auto empty = overlay_frame(
        overlay_surface, {}, 1, true);
    auto output = compositor.compose(input, empty, 1);
    expect(output && output->passthrough
            && output->texture == base_texture,
        "empty overlay passes source NV12 through");
    auto snapshot = diagnostics.snapshot();
    expect(snapshot.counter(CastPipelineCounter::compute_dispatches) == 0,
        "empty overlay dispatch count is zero");
    const auto copies_before = snapshot.counter(
        CastPipelineCounter::gpu_copies);
    output.reset();

    const CastOverlayRect rect{8, 8, 16, 8};
    fill_overlay_rect(overlay_surface, rect,
        Color{1.0F, 1.0F, 1.0F, 1.0F});
    auto white = overlay_frame(overlay_surface, rect, 2);
    output = compositor.compose(input, white, 2);
    expect(output && !output->passthrough,
        "visible overlay produces a composed NV12 surface");
    auto pixels = read_nv12(device, context,
        static_cast<ID3D11Texture2D*>(output->texture));
    expect_near(luma(pixels, 10, 10), 235, 2,
        "opaque white overlay produces limited-range white");
    expect_near(chroma_u(pixels, 5, 5), 128, 2,
        "white overlay keeps neutral U");
    expect_near(chroma_v(pixels, 5, 5), 128, 2,
        "white overlay keeps neutral V");
    expect(luma(pixels, 2, 2) == 64,
        "pixels outside dirty rect retain source luma");
    snapshot = diagnostics.snapshot();
    expect(snapshot.counter(CastPipelineCounter::compute_dispatches) == 2,
        "visible overlay performs one Y and one UV dispatch");
    expect(snapshot.counter(CastPipelineCounter::gpu_copies)
            == copies_before
                + (compositor_config.separate_encoder_texture ? 2U : 1U),
        "visible overlay copy count matches selected encoder topology");

    auto cached = compositor.compose(input, white, 3);
    expect(cached == output, "unchanged source and overlay reuse output");
    expect(diagnostics.snapshot().counter(
        CastPipelineCounter::compute_dispatches) == 2,
        "cached output performs no extra dispatch");
    cached.reset();
    output.reset();

    clear_overlay(overlay_surface);
    const CastOverlayRect block{8, 8, 2, 2};
    fill_overlay_rect(overlay_surface, {8, 8, 1, 1},
        Color{1.0F, 0.0F, 0.0F, 1.0F});
    auto red_pixel = overlay_frame(overlay_surface, block, 3);
    output = compositor.compose(input, red_pixel, 4);
    expect(static_cast<bool>(output), "single-pixel chroma test compose");
    pixels = read_nv12(device, context,
        static_cast<ID3D11Texture2D*>(output->texture));
    expect_near(luma(pixels, 8, 8), 63, 2,
        "opaque red pixel produces BT.709 limited luma");
    expect(luma(pixels, 9, 8) == 64,
        "transparent neighbor retains source luma");
    expect_near(chroma_u(pixels, 4, 4), 122, 2,
        "UV shader averages red alpha over the 2x2 block");
    expect_near(chroma_v(pixels, 4, 4), 156, 2,
        "UV shader avoids full-strength one-pixel chroma halo");
    output.reset();

    clear_overlay(overlay_surface);
    fill_overlay_rect(overlay_surface, block,
        Color{0.0F, 0.0F, 0.0F, 0.5F});
    auto faded_black = overlay_frame(overlay_surface, block, 4);
    output = compositor.compose(input, faded_black, 5);
    expect(static_cast<bool>(output), "premultiplied fade compose");
    pixels = read_nv12(device, context,
        static_cast<ID3D11Texture2D*>(output->texture));
    expect_near(luma(pixels, 8, 8), 40, 2,
        "half-alpha black blends limited-range offsets correctly");
    expect_near(chroma_u(pixels, 4, 4), 128, 2,
        "half-alpha black keeps neutral U");
    expect_near(chroma_v(pixels, 4, 4), 128, 2,
        "half-alpha black keeps neutral V");

    expect(diagnostics.snapshot().event(
        CastPipelineEvent::nv12_compose).count == 3,
        "diagnostics count only real NV12 compositions");
    output.reset();

    compositor.reset();
    compositor_config.encoder_ready_empty_output = true;
    expect(compositor.configure(device, compositor_config, &diagnostics),
        "configure encoder-ready empty overlay output");
    auto encoder_ready_empty = compositor.compose(input, empty, 6);
    expect(encoder_ready_empty && !encoder_ready_empty->passthrough
            && encoder_ready_empty->copied_to_encoder,
        "production empty overlay returns an encoder-ready NV12 surface");
    pixels = read_nv12(device, context,
        static_cast<ID3D11Texture2D*>(encoder_ready_empty->texture));
    expect(luma(pixels, 2, 2) == 64,
        "encoder-ready empty overlay preserves source luma");
    expect(diagnostics.snapshot().counter(
            CastPipelineCounter::compute_dispatches) == 6,
        "encoder-ready empty overlay performs no compute dispatch");
    encoder_ready_empty.reset();
    compositor.reset();
    overlay_surface = {};
    base_lease.reset();
    base_pool.reset();
    release(multithread);
    release(context);
    release(device);
    CoUninitialize();
    std::cout << "D3D11 NV12 overlay compositor tests passed.\n";
}
