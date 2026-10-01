#pragma once

#include "kirakara/show/types.hpp"
#include "kirakara/show/win32/d3d11_texture_surface.hpp"

#include <cstdint>
#include <memory>

namespace kirakara::show::win32 {

struct StageCanvas {
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    std::uint32_t frame_rate_num{60};
    std::uint32_t frame_rate_den{1};
};

struct PixelRect {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

struct StageCompositorFrame {
    StageCanvas canvas;
    PixelRect video_rect;
    void* overlay_render_target{};
    void* overlay_texture{};
    void* overlay_dxgi_surface{};
    void* d3d_device{};
};

// Fixed-canvas Stage output boundary.  This class does not own playback time
// and must be driven by ShowHost PlaybackSession; it only prepares the
// compositor targets for a single program frame.
class StageCompositor {
public:
    StageCompositor();
    ~StageCompositor();

    StageCompositor(const StageCompositor&) = delete;
    StageCompositor& operator=(const StageCompositor&) = delete;
    StageCompositor(StageCompositor&&) noexcept;
    StageCompositor& operator=(StageCompositor&&) noexcept;

    [[nodiscard]] bool configure(
        const StageCanvas& canvas, void* native_d3d11_device = nullptr);
    [[nodiscard]] const StageCanvas& canvas() const noexcept;

    // Keep the eager policy for generic users. Dedicated pipelines may defer
    // the D2D interoperability flush until a real D2D -> D3D boundary, then
    // call overlay_surface().flush_d2d() before the first D3D consumer.
    void set_defer_d2d_interop_flush(bool defer) noexcept;

    [[nodiscard]] bool begin_frame(
        std::uint32_t source_width, std::uint32_t source_height);
    [[nodiscard]] bool begin_frame_on(D3D11TextureSurface& target,
        std::uint32_t source_width, std::uint32_t source_height,
        Color clear_color = Color{0.0F, 0.0F, 0.0F, 0.0F});
    // Selects and normalizes a target without replacing its pixels. This is
    // used after a full-surface GPU copy of a cached video base, before
    // dynamic title/lyric overlays are drawn.
    [[nodiscard]] bool prepare_frame_on(D3D11TextureSurface& target,
        std::uint32_t source_width, std::uint32_t source_height);
    [[nodiscard]] StageCompositorFrame current_frame() noexcept;

    [[nodiscard]] static PixelRect letterbox_rect(
        std::uint32_t source_width,
        std::uint32_t source_height,
        const StageCanvas& canvas = {});

    [[nodiscard]] bool clear_overlay(
        Color color = Color{0.0F, 0.0F, 0.0F, 0.0F});

    // Composite a video frame (D3D11 BGRA8 texture) into the overlay surface
    // at the letterbox position.  The texture must have
    // D3D11_BIND_SHADER_RESOURCE.  Call after begin_frame() and before
    // drawing subtitles.
    [[nodiscard]] bool composite_video_texture(
        void* d3d11_texture_2d,
        std::uint32_t source_width,
        std::uint32_t source_height);

    // Upload BGRA8 CPU pixels as video frame for testing/diagnostics.
    [[nodiscard]] bool upload_video_bgra(
        const void* pixels,
        std::uint32_t source_width,
        std::uint32_t source_height);

    [[nodiscard]] D3D11TextureSurface& overlay_surface() noexcept;
    [[nodiscard]] const D3D11TextureSurface& overlay_surface() const noexcept;

private:
    struct SourceBitmapCache;

    [[nodiscard]] bool prepare_active_surface();

    StageCanvas canvas_;
    PixelRect video_rect_;
    D3D11TextureSurface overlay_;
    D3D11TextureSurface* active_surface_{&overlay_};
    std::unique_ptr<SourceBitmapCache> source_bitmap_cache_;
    bool defer_d2d_interop_flush_{};
};

} // namespace kirakara::show::win32
