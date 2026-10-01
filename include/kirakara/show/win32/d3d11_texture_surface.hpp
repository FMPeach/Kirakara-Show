#pragma once

#include "kirakara/show/types.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace kirakara::show::win32 {

// D3D11 BGRA8 texture with a Direct2D device-context target. Native handles
// are borrowed ID2D1DeviceContext* and ID3D11Texture2D* respectively.
class D3D11TextureSurface {
public:
    D3D11TextureSurface();
    ~D3D11TextureSurface();

    D3D11TextureSurface(const D3D11TextureSurface&) = delete;
    D3D11TextureSurface& operator=(const D3D11TextureSurface&) = delete;
    D3D11TextureSurface(D3D11TextureSurface&&) noexcept;
    D3D11TextureSurface& operator=(D3D11TextureSurface&&) noexcept;

    [[nodiscard]] bool resize(std::uint32_t width, std::uint32_t height);
    // Creates the target on an existing D3D11 device. This lets every frame
    // slot and decoded video texture stay on one GPU without copies.
    [[nodiscard]] bool resize_on_device(void* native_d3d11_device,
        std::uint32_t width, std::uint32_t height);
    // Same as resize_on_device, but requests a DXGI-shareable texture. Native
    // Stage/Cast can keep using the resource directly while Flutter opens the
    // shared handle on its own D3D device.
    [[nodiscard]] bool resize_shared_on_device(void* native_d3d11_device,
        std::uint32_t width, std::uint32_t height);
    // Creates a shareable texture and a separate drawing context in the same
    // Direct2D resource domain as another surface. Device-dependent glyph
    // masks, brushes and layers can then be reused while frame-pool targets
    // rotate.
    [[nodiscard]] bool resize_shared_in_resource_domain(
        const D3D11TextureSurface& resource_domain,
        std::uint32_t width, std::uint32_t height);
    [[nodiscard]] void* native_render_target() noexcept;
    [[nodiscard]] void* native_d3d_device() noexcept;
    [[nodiscard]] void* native_dxgi_surface() noexcept;
    [[nodiscard]] void* native_texture() noexcept;
    [[nodiscard]] const void* native_texture() const noexcept;

    // Copies every pixel from another same-device, same-size BGRA8 surface.
    // The caller must finish/flush any preceding Direct2D writes to source.
    // This only records CopyResource; it deliberately does not Flush the
    // D3D11 immediate context.
    [[nodiscard]] bool copy_from(const D3D11TextureSurface& source) noexcept;

    // Simple BeginDraw / Clear / EndDraw wrappers for callers that need
    // to clear the surface before handing it to LyricRenderer.
    void begin_draw();
    [[nodiscard]] bool end_draw();
    void clear(Color color);
    // Replaces only this physical-pixel rectangle, including alpha. The
    // operation preserves the caller's transform and primitive blend mode and
    // is intended for transparent dirty-region erasure between overlay draws.
    [[nodiscard]] bool clear_rect(float left, float top, float right,
        float bottom, Color color = Color{0.0F, 0.0F, 0.0F, 0.0F});
    // Flush D2D command batch to the D3D11 immediate context.  Call after
    // EndDraw if the next operation uses D3D11 directly on the same texture.
    void flush_d2d();
    // Submit the immediate-context command buffer to the GPU. This is needed
    // before a texture is published to a consumer on another D3D11 device
    // when there is no swap-chain Present boundary to perform the submit.
    void flush_d3d();

    // Premultiplied RGBA8 readback for diagnostics/tests.
    [[nodiscard]] std::vector<std::uint8_t> read_rgba8() const;
    // Zero-copy BGRA8 readback into pre-allocated buffer
    // (size >= width*height*4).  Uses cached staging texture.
    [[nodiscard]] bool read_bgra8_into(void* dst) const;
    [[nodiscard]] bool synchronize() const;

    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] bool using_hardware_device() const noexcept;

    // Releases the Direct2D writer side on its owner thread while preserving
    // the D3D11 texture for outstanding immutable Stage Sink leases.
    void retire_producer_access() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kirakara::show::win32
