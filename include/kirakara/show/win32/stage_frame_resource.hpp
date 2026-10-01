#pragma once

#include "kirakara/show/stage_frame.hpp"
#include "kirakara/show/win32/d3d11_texture_surface.hpp"

#include <memory>

namespace kirakara::show::win32 {

// GPU backing for one StageFramePool slot. Every slot is created on the
// producer's D3D11 device so video, overlays, presenters and encoders can stay
// on the GPU. Cross-device sharing is added at the Sink boundary in Phase 5.
class D3D11StageFrameResource final : public StageFrameResource {
public:
    [[nodiscard]] static std::shared_ptr<D3D11StageFrameResource> create(
        const StageOutputProfile& profile, void* native_d3d11_device);
    [[nodiscard]] static std::shared_ptr<D3D11StageFrameResource> create(
        const StageOutputProfile& profile,
        const D3D11TextureSurface& resource_domain);

    [[nodiscard]] void* native_resource() noexcept override;
    [[nodiscard]] std::uintptr_t shared_handle() const noexcept override;
    void retire_producer_access() noexcept override;

    [[nodiscard]] D3D11TextureSurface& surface() noexcept;
    [[nodiscard]] const D3D11TextureSurface& surface() const noexcept;

private:
    D3D11TextureSurface surface_;
    std::uintptr_t shared_handle_{};
};

} // namespace kirakara::show::win32
