#include "../apps/show_host/cast/cast_nv12_surface_pool.h"
#include "../apps/show_host/cast/cast_pipeline_diagnostics.h"
#include "../apps/show_host/cast/d3d11_cast_capabilities.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

#include <d3d10.h>
#include <d3d11.h>

namespace {

template <typename T>
void release(T*& value) {
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

}  // namespace

int main() {
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    D3D_FEATURE_LEVEL level{};
    auto hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context);
    }
    expect(SUCCEEDED(hr) && device, "create D3D11 test device");
    ID3D10Multithread* multithread{};
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    D3D11CastCapabilityProbeOptions options;
    options.probe_hardware_encoder = false;
    const auto report = D3D11CastCapabilityProbe::run(device, options);
    if (!report.nv12_plane_uavs.passed()
            || (!report.uav_video_encoder_bind.passed()
                && !report.compose_to_encoder_copy.passed())) {
        std::cout << "SKIP: device has no usable NV12 UAV pool path\n";
        release(multithread);
        release(context);
        release(device);
        return EXIT_SUCCESS;
    }

    CastPipelineDiagnostics diagnostics;
    CastNv12SurfacePool pool;
    CastNv12SurfacePoolConfig config;
    config.width = options.width;
    config.height = options.height;
    config.capacity = 3;
    config.create_plane_uavs = true;
    config.encoder_compatible = true;
    config.separate_encoder_texture =
        !report.uav_video_encoder_bind.passed();
    expect(pool.configure(device, config, &diagnostics),
        "configure preallocated NV12 pool");
    expect(pool.capacity() == 3 && pool.available() == 3,
        "pool publishes fixed capacity");
    expect(pool.textures_created()
            == (config.separate_encoder_texture ? 6U : 3U),
        "pool creates textures only during configure");
    expect(pool.views_created() == 6,
        "pool creates two plane UAVs per slot");

    auto first = pool.try_acquire();
    auto second = pool.try_acquire();
    auto third = pool.try_acquire();
    expect(first && second && third && !pool.try_acquire(),
        "pool never aliases an in-flight slot");
    auto retained = first.retention_token();
    first.reset();
    expect(pool.available() == 0,
        "retention token spans asynchronous ownership");
    retained.reset();
    expect(pool.available() == 1,
        "slot returns only after final retention token");

    auto replacement = pool.try_acquire();
    expect(static_cast<bool>(replacement), "released slot can be reacquired");
    auto* retained_texture = replacement.compose_texture();
    pool.reset();
    expect(pool.retired() && pool.capacity() == 0,
        "reset retires the active generation");
    expect(replacement.compose_texture() == retained_texture,
        "retired generation remains alive through outstanding lease");
    replacement.reset();
    second.reset();
    third.reset();

    const auto snapshot = diagnostics.snapshot();
    expect(snapshot.counter(CastPipelineCounter::texture_creations)
            == (config.separate_encoder_texture ? 6U : 3U),
        "pool reports bounded texture creation");
    expect(snapshot.counter(CastPipelineCounter::view_creations) == 6,
        "pool reports bounded view creation");

    release(multithread);
    release(context);
    release(device);
    return EXIT_SUCCESS;
}
