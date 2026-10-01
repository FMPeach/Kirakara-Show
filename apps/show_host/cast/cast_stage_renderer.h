#pragma once

#include "kirakara/show/win32/stage_compositor.hpp"

#include <cstdint>

// Offscreen renderer for the Cast flavor of the shared Stage program.
// PlaybackSession owns time; the decoder owns source decoding; this class
// only composites a supplied D3D11 texture and Stage overlays.
class CastStageRenderer {
public:
    CastStageRenderer();

    [[nodiscard]] bool configure(
        const kirakara::show::win32::StageCanvas& canvas);

    [[nodiscard]] bool compose_idle_frame();
    [[nodiscard]] bool compose_video_texture(
        void* d3d11_texture_2d,
        std::uint32_t source_width,
        std::uint32_t source_height);

    // Submit all D2D work recorded for the current Cast frame before its BGRA
    // texture is consumed by the D3D11 video processor.
    void finalize_frame();

    [[nodiscard]] kirakara::show::win32::StageCompositor& compositor() noexcept;
    [[nodiscard]] const kirakara::show::win32::StageCompositor& compositor()
        const noexcept;
    [[nodiscard]] void* native_d3d_device() noexcept;

private:
    kirakara::show::win32::StageCompositor compositor_;
};
