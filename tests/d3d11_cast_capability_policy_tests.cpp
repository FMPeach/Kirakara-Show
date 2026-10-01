#include "../apps/show_host/cast/d3d11_cast_capabilities.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

D3D11CastCapabilityCheck yes() {
    return {true, true, 0};
}

D3D11CastCapabilityCheck no() {
    return {true, false, -1};
}

D3D11CastCapabilityReport baseline() {
    D3D11CastCapabilityReport report;
    report.device_healthy = yes();
    report.native_nv12_source = yes();
    report.nv12_plane_srvs = yes();
    report.nv12_plane_uavs = yes();
    report.uav_video_encoder_bind = yes();
    report.combined_uav_encoder_input = yes();
    report.nv12_video_processor = yes();
    report.dxgi_h264_encoder = yes();
    report.tracked_sample_release = yes();
    report.even_420_geometry = yes();
    return report;
}

}  // namespace

int main() {
    auto selected = select_d3d11_cast_backend(baseline());
    expect(selected.backend == D3D11CastBackend::compute_nv12_a,
        "complete probe should select backend A");
    expect(!selected.backend_active,
        "standalone policy selection must not activate a runtime backend");
    expect(!selected.requires_encoder_copy,
        "combined encoder surface should not require a copy");

    auto copy = baseline();
    copy.uav_video_encoder_bind = no();
    copy.combined_uav_encoder_input = no();
    copy.compose_to_encoder_copy = yes();
    selected = select_d3d11_cast_backend(copy);
    expect(selected.backend == D3D11CastBackend::compute_nv12_a,
        "verified copy bridge should retain backend A");
    expect(selected.requires_encoder_copy,
        "copy bridge must be fixed at startup");

    auto no_uav = baseline();
    no_uav.nv12_plane_uavs = no();
    selected = select_d3d11_cast_backend(no_uav);
    expect(selected.backend == D3D11CastBackend::native_nv12_vp_b,
        "missing plane UAV should select backend B");

    auto no_native = baseline();
    no_native.native_nv12_source = no();
    selected = select_d3d11_cast_backend(no_native);
    expect(selected.backend == D3D11CastBackend::compatibility_c,
        "missing native NV12 decode should select backend C");

    auto no_retention = baseline();
    no_retention.tracked_sample_release = no();
    selected = select_d3d11_cast_backend(no_retention);
    expect(selected.backend == D3D11CastBackend::compatibility_c
            && selected.reason
                == D3D11CastBackendReason::encoder_sample_retention_unavailable,
        "untracked asynchronous MFT ownership must select backend C");

    auto no_normalizer = baseline();
    no_normalizer.nv12_video_processor = no();
    selected = select_d3d11_cast_backend(no_normalizer);
    expect(selected.backend == D3D11CastBackend::compatibility_c
            && selected.reason
                == D3D11CastBackendReason::video_processor_unavailable,
        "A must support non-1080p NV12 source normalization");

    auto incomplete = baseline();
    incomplete.native_nv12_source = {};
    selected = select_d3d11_cast_backend(incomplete);
    expect(selected.backend == D3D11CastBackend::compatibility_c
            && selected.reason == D3D11CastBackendReason::probe_incomplete,
        "an unexecuted mandatory probe must never select A optimistically");

    auto preflight = baseline();
    preflight.native_nv12_source = {};
    preflight.combined_uav_encoder_input = {};
    preflight.dxgi_h264_encoder = {};
    preflight.tracked_sample_release = {};
    selected = select_d3d11_cast_preflight(preflight);
    expect(selected.backend == D3D11CastBackend::compute_nv12_a,
        "resource preflight should defer slow decoder and encoder activation");
    expect(!selected.backend_active,
        "resource preflight must not publish an active backend");

    preflight.uav_video_encoder_bind = no();
    preflight.compose_to_encoder_copy = yes();
    selected = select_d3d11_cast_preflight(preflight);
    expect(selected.backend == D3D11CastBackend::compute_nv12_a
            && selected.requires_encoder_copy,
        "resource preflight should select the verified GPU copy bridge");

    preflight.compose_to_encoder_copy = no();
    selected = select_d3d11_cast_preflight(preflight);
    expect(selected.backend == D3D11CastBackend::compatibility_c,
        "resource preflight must roll down when no encoder surface exists");
    return 0;
}
