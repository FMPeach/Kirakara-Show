#pragma once

#include "cast_overlay_renderer.h"

#include <cstddef>
#include <cstdint>
#include <memory>

class CastPipelineDiagnostics;

struct D3D11Nv12OverlayCompositorConfig {
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    std::size_t surface_count{8};
    bool separate_encoder_texture{};
    // Production Cast requires an encoder-bindable texture even when the
    // overlay is empty. In that mode the compositor performs one cached copy
    // per new base frame but still performs no compute dispatch.
    bool encoder_ready_empty_output{};
};

struct D3D11Nv12OverlayInput {
    void* texture{};
    void* y_srv{};
    void* uv_srv{};
    std::shared_ptr<void> retention;
    std::uint64_t frame_id{};
    std::uint64_t generation{};
};

struct D3D11Nv12CompositedFrame {
    void* texture{};
    std::shared_ptr<void> retention;
    std::uint64_t source_frame_id{};
    std::uint64_t source_generation{};
    std::uint64_t overlay_content_version{};
    bool passthrough{};
    bool copied_to_encoder{};
};

enum class D3D11Nv12OverlayFailure : std::uint32_t {
    none,
    not_configured,
    invalid_input,
    device_mismatch,
    overlay_not_prepared,
    surface_unavailable,
    shader_creation_failed,
};

// Cast-only NV12 compositor. The source resource is read-only and a different
// pooled NV12 resource is used for UAV writes, as required by D3D11 video
// resource hazard rules. Empty overlays never dispatch; library callers may
// pass the normalized source through, while production can request one cached
// copy into an encoder-bindable output surface.
class D3D11Nv12OverlayCompositor {
public:
    D3D11Nv12OverlayCompositor();
    ~D3D11Nv12OverlayCompositor();

    D3D11Nv12OverlayCompositor(
        const D3D11Nv12OverlayCompositor&) = delete;
    D3D11Nv12OverlayCompositor& operator=(
        const D3D11Nv12OverlayCompositor&) = delete;

    [[nodiscard]] bool configure(
        void* d3d11_device,
        const D3D11Nv12OverlayCompositorConfig& config = {},
        CastPipelineDiagnostics* diagnostics = nullptr);
    void reset() noexcept;

    // Creates and retains the one BGRA SRV used by the steady-state path.
    // Call after the Cast overlay texture has been configured.
    [[nodiscard]] bool prepare_overlay_texture(void* bgra_texture);

    [[nodiscard]] std::shared_ptr<const D3D11Nv12CompositedFrame> compose(
        const D3D11Nv12OverlayInput& base,
        const CastOverlayFrame& overlay,
        std::uint64_t correlation_id = 0);

    [[nodiscard]] D3D11Nv12OverlayFailure last_failure() const noexcept;
    [[nodiscard]] std::int32_t last_hresult() const noexcept;
    [[nodiscard]] std::size_t available_surfaces() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
