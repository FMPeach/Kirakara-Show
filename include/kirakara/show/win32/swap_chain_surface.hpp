#pragma once

#include <cstdint>
#include <memory>

namespace kirakara::show::win32 {

// Win32 preview surface backed by a DXGI flip-model swap chain and a Direct2D
// device context target. Native render target is ID2D1DeviceContext*.
class SwapChainSurface {
public:
    SwapChainSurface();
    ~SwapChainSurface();

    SwapChainSurface(const SwapChainSurface&) = delete;
    SwapChainSurface& operator=(const SwapChainSurface&) = delete;
    SwapChainSurface(SwapChainSurface&&) noexcept;
    SwapChainSurface& operator=(SwapChainSurface&&) noexcept;

    [[nodiscard]] bool initialize(void* hwnd);
    [[nodiscard]] bool resize(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] void* native_render_target() noexcept;
    [[nodiscard]] void* native_d3d_device() noexcept;
    [[nodiscard]] void* native_dxgi_surface() noexcept;
    [[nodiscard]] bool present(bool wait_for_vsync = true);
    void shutdown();

    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] bool using_hardware_device() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kirakara::show::win32
