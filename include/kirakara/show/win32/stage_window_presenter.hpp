#pragma once

#include "kirakara/show/stage_frame.hpp"

#include <cstdint>
#include <memory>

namespace kirakara::show::win32 {

// Presents immutable Unified Stage frames to one Win32 HWND. The source
// texture, optional preview scaling and swap-chain back buffer stay on the
// producer's D3D11 device; no CPU pixel copy is performed.
class StageWindowPresenter {
public:
    StageWindowPresenter();
    ~StageWindowPresenter();

    StageWindowPresenter(const StageWindowPresenter&) = delete;
    StageWindowPresenter& operator=(const StageWindowPresenter&) = delete;
    StageWindowPresenter(StageWindowPresenter&&) noexcept;
    StageWindowPresenter& operator=(StageWindowPresenter&&) noexcept;

    [[nodiscard]] bool initialize(void* hwnd, void* native_d3d11_device);
    [[nodiscard]] bool resize(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] bool present(
        const StageFrameLease& frame, bool wait_for_vsync = true);
    [[nodiscard]] bool repeat_last(bool wait_for_vsync = true);
    [[nodiscard]] bool present_black(bool wait_for_vsync = true);

    // Rebuilds only the HWND swap-chain target while retaining the D3D device
    // and last immutable Stage frame. Used for transient DXGI/D2D failures.
    [[nodiscard]] bool recover_output();

    // Releases the retained immutable frame before a StageFramePool profile
    // is rebuilt. The swap chain and D3D device remain alive.
    void release_frame();
    void shutdown();

    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    // Changes whenever the HWND swap-chain target is recreated/resized. A
    // retained static Stage frame must be submitted once to each revision.
    [[nodiscard]] std::uint64_t output_revision() const noexcept;
    [[nodiscard]] std::uint64_t presented_frames() const noexcept;
    [[nodiscard]] StageFrameTiming last_timing() const noexcept;
    [[nodiscard]] std::int32_t last_error_code() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kirakara::show::win32
