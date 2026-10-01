#include "../apps/show_host/cast/d3d11_cast_capabilities.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <iterator>

#include <d3d10.h>
#include <d3d11.h>
#include <mfapi.h>
#include <objbase.h>

namespace {

template <typename T>
void release(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

void print_check(const char* name, const D3D11CastCapabilityCheck& check) {
    std::cout << "  \"" << name << "\": {\"attempted\": "
              << (check.attempted ? "true" : "false")
              << ", \"supported\": "
              << (check.supported ? "true" : "false")
              << ", \"hresult\": \"0x" << std::hex << std::setw(8)
              << std::setfill('0')
              << static_cast<std::uint32_t>(check.hresult)
              << std::dec << "\"}";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto mf = MFStartup(MF_VERSION);
    if (FAILED(mf)) return EXIT_FAILURE;

    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    D3D_FEATURE_LEVEL feature_level{};
    const D3D_FEATURE_LEVEL requested[]{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    auto hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        requested, static_cast<UINT>(std::size(requested)),
        D3D11_SDK_VERSION, &device, &feature_level, &context);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT
                | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            requested + 1, 1,
            D3D11_SDK_VERSION, &device, &feature_level, &context);
    }
    if (FAILED(hr)) {
        std::cerr << "D3D11CreateDevice failed: 0x" << std::hex
                  << static_cast<std::uint32_t>(hr) << '\n';
        MFShutdown();
        if (SUCCEEDED(com)) CoUninitialize();
        return EXIT_FAILURE;
    }
    ID3D10Multithread* multithread{};
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    D3D11CastCapabilityProbeOptions options;
    options.decoder_source = argc > 1 ? argv[1] : nullptr;
    const auto report = D3D11CastCapabilityProbe::run(device, options);
    std::cout << "{\n"
              << "  \"adapter_luid\": \"0x" << std::hex
              << report.adapter_luid << std::dec << "\",\n"
              << "  \"vendor_id\": " << report.vendor_id << ",\n"
              << "  \"device_id\": " << report.device_id << ",\n"
              << "  \"feature_level\": " << report.feature_level << ",\n"
              << "  \"windows_build\": " << report.windows_build << ",\n"
              << "  \"backend\": " << static_cast<unsigned>(report.backend)
              << ",\n"
              << "  \"reason\": " << static_cast<unsigned>(report.reason)
              << ",\n"
              << "  \"requires_encoder_copy\": "
              << (report.requires_encoder_copy ? "true" : "false")
              << ",\n  \"encoder_retains_samples_async\": "
              << (report.encoder_retains_samples_async ? "true" : "false")
              << ",\n  \"force_keyframe_control\": "
              << static_cast<unsigned>(report.force_keyframe_control)
              << ",\n";
    print_check("device_healthy", report.device_healthy);
    std::cout << ",\n";
    print_check("native_nv12_source", report.native_nv12_source);
    std::cout << ",\n";
    print_check("nv12_plane_srvs", report.nv12_plane_srvs);
    std::cout << ",\n";
    print_check("nv12_plane_uavs", report.nv12_plane_uavs);
    std::cout << ",\n";
    print_check("uav_video_encoder_bind", report.uav_video_encoder_bind);
    std::cout << ",\n";
    print_check("combined_uav_encoder_input",
        report.combined_uav_encoder_input);
    std::cout << ",\n";
    print_check("compose_to_encoder_copy", report.compose_to_encoder_copy);
    std::cout << ",\n";
    print_check("nv12_video_processor", report.nv12_video_processor);
    std::cout << ",\n";
    print_check("dxgi_h264_encoder", report.dxgi_h264_encoder);
    std::cout << ",\n";
    print_check("tracked_sample_release", report.tracked_sample_release);
    std::cout << ",\n";
    print_check("even_420_geometry", report.even_420_geometry);
    std::cout << "\n}\n";

    release(multithread);
    release(context);
    release(device);
    MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    return EXIT_SUCCESS;
}
