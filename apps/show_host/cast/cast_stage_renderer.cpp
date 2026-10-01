#include "cast_stage_renderer.h"

#include <cstddef>

CastStageRenderer::CastStageRenderer() {
    // Cast draws the background, video, title, and lyrics into one texture
    // before crossing to D3D11/NV12. Avoid flushing between those D2D batches.
    // Unified Stage owns an equivalent boundary policy independently.
    compositor_.set_defer_d2d_interop_flush(true);
}

bool CastStageRenderer::configure(
        const kirakara::show::win32::StageCanvas& canvas) {
    return compositor_.configure(canvas);
}

bool CastStageRenderer::compose_idle_frame() {
    constexpr std::uint32_t width = 16;
    constexpr std::uint32_t height = 9;
    std::uint8_t pixels[width * height * 4U]{};
    for (std::size_t i = 0; i < sizeof(pixels); i += 4) {
        pixels[i] = 12;
        pixels[i + 1] = 12;
        pixels[i + 2] = 14;
        pixels[i + 3] = 255;
    }
    return compositor_.begin_frame(width, height)
        && compositor_.upload_video_bgra(pixels, width, height);
}

bool CastStageRenderer::compose_video_texture(
        void* d3d11_texture_2d,
        std::uint32_t source_width,
        std::uint32_t source_height) {
    return compositor_.begin_frame(source_width, source_height)
        && compositor_.composite_video_texture(
            d3d11_texture_2d, source_width, source_height);
}

void CastStageRenderer::finalize_frame() {
    compositor_.overlay_surface().flush_d2d();
}

kirakara::show::win32::StageCompositor&
CastStageRenderer::compositor() noexcept {
    return compositor_;
}

const kirakara::show::win32::StageCompositor&
CastStageRenderer::compositor() const noexcept {
    return compositor_;
}

void* CastStageRenderer::native_d3d_device() noexcept {
    return compositor_.current_frame().d3d_device;
}
