#pragma once

#include "kirakara/show/types.hpp"
#include "kirakara/show/renderer.hpp"
#include "kirakara/show/render_style.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kirakara::show::win32 {

class LyricRenderer final : public kirakara::show::IRenderer {
public:
    LyricRenderer();
    ~LyricRenderer() override;

    LyricRenderer(const LyricRenderer&) = delete;
    LyricRenderer& operator=(const LyricRenderer&) = delete;
    LyricRenderer(LyricRenderer&&) noexcept;
    LyricRenderer& operator=(LyricRenderer&&) noexcept;

    [[nodiscard]] bool initialize() override;
    void shutdown() override;

    void set_font_family(std::wstring family) override;
    void set_font_families(std::vector<std::wstring> families);
    [[nodiscard]] const std::wstring& font_family() const noexcept override;
    [[nodiscard]] const std::vector<std::wstring>& font_families() const noexcept;
    [[nodiscard]] RendererBackendInfo backend_info() const noexcept override;
    [[nodiscard]] bool attach_target(const NativeRenderTarget& target) override;

    // Native handles are ID2D1RenderTarget* and HWND respectively.  They are
    // opaque here so consumers do not need Direct2D headers in public code.
    // The renderer only draws subtitle glyphs — the caller owns the
    // background (video / image / solid colour) and must clear / fill the
    // render target before calling render().
    [[nodiscard]] bool attach_native_target(void* render_target);
    void resize_window_target(unsigned width, unsigned height);

    [[nodiscard]] bool render(const PreparedDocument& document,
        Seconds current_time, const EngineConfig& config,
        const RenderStyle& style) override;
    [[nodiscard]] bool render_overlay(const PreparedDocument& document,
        Seconds current_time, const EngineConfig& config,
        const RenderStyle& style);
    // Draw a caller-evaluated frame. Cast uses this to share one timeline
    // evaluation between visibility/version tracking and glyph rendering.
    [[nodiscard]] bool render_overlay_frame(
        const PreparedDocument& document,
        const FrameState& frame,
        Seconds current_time,
        const EngineConfig& config,
        const RenderStyle& style);
    [[nodiscard]] bool render_overlay_window(void* hwnd,
        const PreparedDocument& document, Seconds current_time,
        const EngineConfig& config, const RenderStyle& style);
    [[nodiscard]] bool render_window(void* hwnd,
        const PreparedDocument& document, Seconds current_time,
        const EngineConfig& config, const RenderStyle& style);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kirakara::show::win32
