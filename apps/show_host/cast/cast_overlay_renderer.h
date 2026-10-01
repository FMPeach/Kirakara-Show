#pragma once

#include "kirakara/show/render_style.hpp"
#include "kirakara/show/song_title.hpp"
#include "kirakara/show/types.hpp"
#include "kirakara/show/win32/d3d11_texture_surface.hpp"
#include "kirakara/show/win32/lyric_renderer.hpp"
#include "kirakara/show/win32/song_title_renderer.hpp"

#include <cstdint>
#include <memory>

class CastPipelineDiagnostics;

struct CastOverlayRect {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};

    [[nodiscard]] constexpr bool empty() const noexcept {
        return width == 0 || height == 0;
    }
};

struct CastOverlayFrame {
    void* texture{};
    std::uint32_t width{};
    std::uint32_t height{};
    CastOverlayRect dirty_rect;
    CastOverlayRect coverage_rect;
    std::uint64_t content_version{};
    bool empty{true};
    bool changed{};
    bool title_drawn{};
    bool lyrics_drawn{};
};

// Cast-only transparent BGRA overlay target. Timeline state is evaluated once
// per request and reused by LyricRenderer; unchanged/static overlay frames do
// not issue any Direct2D work.
class CastOverlayRenderer {
public:
    CastOverlayRenderer();
    ~CastOverlayRenderer();

    CastOverlayRenderer(const CastOverlayRenderer&) = delete;
    CastOverlayRenderer& operator=(const CastOverlayRenderer&) = delete;

    [[nodiscard]] bool configure(
        void* d3d11_device,
        std::uint32_t width,
        std::uint32_t height,
        CastPipelineDiagnostics* diagnostics = nullptr);
    void reset() noexcept;

    [[nodiscard]] bool render(
        const kirakara::show::PreparedDocument& document,
        const kirakara::show::SongTitleConfig& title,
        kirakara::show::Seconds current_time,
        const kirakara::show::EngineConfig& engine,
        const kirakara::show::RenderStyle& style,
        std::uint64_t content_revision,
        kirakara::show::win32::LyricRenderer& lyric_renderer,
        kirakara::show::win32::SongTitleRenderer* title_renderer = nullptr);

    [[nodiscard]] const CastOverlayFrame& frame() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
