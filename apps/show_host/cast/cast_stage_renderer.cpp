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
    idle_background_.reset();
    invalidate_idle_frame();
    return compositor_.configure(canvas);
}

bool CastStageRenderer::compose_idle_frame(bool show_background) {
    idle_frame_reused_ = idle_frame_cached_
        && idle_background_visible_ == show_background;
    if (idle_frame_reused_) return true;
    invalidate_idle_frame();
    if (show_background && idle_background_source_) {
        const auto& canvas = compositor_.canvas();
        auto* device = static_cast<ID3D11Device*>(native_d3d_device());
        if (idle_background_.configure(device, canvas.width, canvas.height,
                *idle_background_source_)) {
            if (!compositor_.begin_frame_on(compositor_.overlay_surface(),
                    canvas.width, canvas.height,
                    {0.0F, 0.0F, 0.0F, 1.0F})
                || !compositor_.composite_video_texture(
                    idle_background_.texture(), canvas.width, canvas.height)) {
                return false;
            }
            idle_frame_cached_ = true;
            idle_background_visible_ = true;
            return true;
        }
    }
    // Preserve the existing plain base for an active audio-only program or
    // a video that is still priming. Those are not the empty-queue screen.
    constexpr std::uint32_t width = 16;
    constexpr std::uint32_t height = 9;
    std::uint8_t pixels[width * height * 4U]{};
    for (std::size_t i = 0; i < sizeof(pixels); i += 4) {
        pixels[i] = 12;
        pixels[i + 1] = 12;
        pixels[i + 2] = 14;
        pixels[i + 3] = 255;
    }
    idle_frame_cached_ = compositor_.begin_frame(width, height)
        && compositor_.upload_video_bgra(pixels, width, height);
    idle_background_visible_ = show_background;
    return idle_frame_cached_;
}

void CastStageRenderer::set_idle_background_source(
        std::shared_ptr<const IdleBackgroundSource> source) {
    if (idle_background_source_ == source) return;
    idle_background_source_ = std::move(source);
    idle_background_.reset();
    invalidate_idle_frame();
}

bool CastStageRenderer::idle_frame_reused() const noexcept {
    return idle_frame_reused_;
}

void CastStageRenderer::invalidate_idle_frame() noexcept {
    idle_frame_cached_ = false;
    idle_frame_reused_ = false;
}

bool CastStageRenderer::compose_video_texture(
        void* d3d11_texture_2d,
        std::uint32_t source_width,
        std::uint32_t source_height) {
    invalidate_idle_frame();
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
