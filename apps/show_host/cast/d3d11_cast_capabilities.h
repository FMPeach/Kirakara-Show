#pragma once

#include <cstdint>

enum class D3D11CastBackend : std::uint32_t {
    compatibility_c = 0,
    native_nv12_vp_b = 1,
    compute_nv12_a = 2,
};

enum class D3D11CastBackendReason : std::uint32_t {
    compute_path_complete,
    compute_encoder_copy_required,
    plane_views_unavailable,
    compute_bind_unavailable,
    native_nv12_unavailable,
    dxgi_encoder_unavailable,
    encoder_sample_retention_unavailable,
    video_processor_unavailable,
    probe_incomplete,
    invalid_device,
};

enum class D3D11CastKeyframeControlStatus : std::uint8_t {
    unknown,
    unsupported,
    supported,
    failed,
};

struct D3D11CastCapabilityCheck {
    bool attempted{};
    bool supported{};
    std::int32_t hresult{};

    [[nodiscard]] constexpr bool passed() const noexcept {
        return attempted && supported;
    }
};

struct D3D11CastCapabilityReport {
    std::uint64_t adapter_luid{};
    std::uint32_t vendor_id{};
    std::uint32_t device_id{};
    std::uint32_t feature_level{};
    std::uint32_t windows_build{};

    D3D11CastCapabilityCheck device_healthy;
    D3D11CastCapabilityCheck native_nv12_source;
    D3D11CastCapabilityCheck nv12_plane_srvs;
    D3D11CastCapabilityCheck nv12_plane_uavs;
    D3D11CastCapabilityCheck uav_video_encoder_bind;
    D3D11CastCapabilityCheck combined_uav_encoder_input;
    D3D11CastCapabilityCheck compose_to_encoder_copy;
    D3D11CastCapabilityCheck nv12_video_processor;
    D3D11CastCapabilityCheck dxgi_h264_encoder;
    D3D11CastCapabilityCheck tracked_sample_release;
    D3D11CastCapabilityCheck even_420_geometry;

    D3D11CastBackend backend{D3D11CastBackend::compatibility_c};
    D3D11CastBackendReason reason{D3D11CastBackendReason::probe_incomplete};
    // A standalone probe reports the best eligible backend but does not make
    // it active. The host sets this only after every component of the selected
    // Cast profile has started and the published backend matches the path that
    // will actually receive frames.
    bool backend_active{};
    bool requires_encoder_copy{};
    bool encoder_retains_samples_async{};
    D3D11CastKeyframeControlStatus force_keyframe_control{
        D3D11CastKeyframeControlStatus::unknown};
};

struct D3D11CastCapabilityProbeOptions {
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    std::uint32_t frame_rate_num{60};
    std::uint32_t frame_rate_den{1};
    const wchar_t* decoder_source{};
    bool probe_hardware_encoder{true};
};

[[nodiscard]] constexpr D3D11CastCapabilityReport
select_d3d11_cast_backend(D3D11CastCapabilityReport report) noexcept {
    report.requires_encoder_copy = false;
    if (!report.device_healthy.passed()) {
        report.backend = D3D11CastBackend::compatibility_c;
        report.reason = D3D11CastBackendReason::invalid_device;
        return report;
    }
    if (!report.native_nv12_source.attempted
            || !report.dxgi_h264_encoder.attempted
            || !report.tracked_sample_release.attempted) {
        report.backend = D3D11CastBackend::compatibility_c;
        report.reason = D3D11CastBackendReason::probe_incomplete;
        return report;
    }
    if (!report.native_nv12_source.passed()) {
        report.backend = D3D11CastBackend::compatibility_c;
        report.reason = D3D11CastBackendReason::native_nv12_unavailable;
        return report;
    }
    if (!report.dxgi_h264_encoder.passed()) {
        report.backend = D3D11CastBackend::compatibility_c;
        report.reason = D3D11CastBackendReason::dxgi_encoder_unavailable;
        return report;
    }
    if (!report.tracked_sample_release.passed()) {
        report.backend = D3D11CastBackend::compatibility_c;
        report.reason =
            D3D11CastBackendReason::encoder_sample_retention_unavailable;
        return report;
    }
    const bool plane_views = report.nv12_plane_srvs.passed()
        && report.nv12_plane_uavs.passed();
    const bool direct_encoder_surface =
        report.uav_video_encoder_bind.passed()
        && report.combined_uav_encoder_input.passed();
    const bool encoder_surface = direct_encoder_surface
        || (report.compose_to_encoder_copy.passed()
            && report.dxgi_h264_encoder.passed());
    // The production A path must also normalize arbitrary source sizes (4K
    // downscale and sub-1080p upscale) without changing backend mid-session.
    // A direct 1080p copy alone is therefore not enough to certify it.
    if (plane_views && encoder_surface
            && report.even_420_geometry.passed()
            && report.nv12_video_processor.passed()) {
        report.backend = D3D11CastBackend::compute_nv12_a;
        report.requires_encoder_copy =
            !direct_encoder_surface;
        report.reason = report.requires_encoder_copy
            ? D3D11CastBackendReason::compute_encoder_copy_required
            : D3D11CastBackendReason::compute_path_complete;
        return report;
    }
    if (report.nv12_video_processor.passed()) {
        report.backend = D3D11CastBackend::native_nv12_vp_b;
        report.reason = !plane_views
            ? D3D11CastBackendReason::plane_views_unavailable
            : D3D11CastBackendReason::compute_bind_unavailable;
        return report;
    }
    report.backend = D3D11CastBackend::compatibility_c;
    report.reason = D3D11CastBackendReason::video_processor_unavailable;
    return report;
}

// Startup preflight deliberately excludes decoder and encoder activation.
// Those operations can enter vendor Media Foundation code for seconds on a
// thermally constrained iGPU. The host uses these cheap D3D11 resource checks
// only to choose which production pipeline to try first; the real decoder and
// encoder then validate that choice and the existing profile loop rolls down
// immediately on failure. Nothing from this decision is persisted.
[[nodiscard]] constexpr D3D11CastCapabilityReport
select_d3d11_cast_preflight(D3D11CastCapabilityReport report) noexcept {
    report.backend_active = false;
    report.requires_encoder_copy = false;
    if (!report.device_healthy.passed()) {
        report.backend = D3D11CastBackend::compatibility_c;
        report.reason = D3D11CastBackendReason::invalid_device;
        return report;
    }
    if (!report.even_420_geometry.passed()
            || !report.nv12_video_processor.passed()) {
        report.backend = D3D11CastBackend::compatibility_c;
        report.reason = D3D11CastBackendReason::video_processor_unavailable;
        return report;
    }
    if (!report.nv12_plane_srvs.passed()
            || !report.nv12_plane_uavs.passed()) {
        report.backend = D3D11CastBackend::compatibility_c;
        report.reason = D3D11CastBackendReason::plane_views_unavailable;
        return report;
    }
    if (report.uav_video_encoder_bind.passed()) {
        report.backend = D3D11CastBackend::compute_nv12_a;
        report.reason = D3D11CastBackendReason::compute_path_complete;
        return report;
    }
    if (report.compose_to_encoder_copy.passed()) {
        report.backend = D3D11CastBackend::compute_nv12_a;
        report.reason =
            D3D11CastBackendReason::compute_encoder_copy_required;
        report.requires_encoder_copy = true;
        return report;
    }
    report.backend = D3D11CastBackend::compatibility_c;
    report.reason = D3D11CastBackendReason::compute_bind_unavailable;
    return report;
}

class D3D11CastCapabilityProbe {
public:
    [[nodiscard]] static D3D11CastCapabilityReport run(
        void* d3d11_device,
        const D3D11CastCapabilityProbeOptions& options = {});
};
