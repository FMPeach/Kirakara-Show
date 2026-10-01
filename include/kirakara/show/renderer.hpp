#pragma once

#include "kirakara/show/types.hpp"
#include "kirakara/show/render_style.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace kirakara::show {

enum class RendererBackend : std::uint8_t {
    unknown,
    direct2d,
    skia,
};

enum class RenderTargetKind : std::uint8_t {
    none,
    direct2d_render_target,
    hwnd,
    skia_canvas,
    rgba8_buffer,
};

struct RendererBackendInfo {
    RendererBackend backend{RendererBackend::unknown};
    std::string_view name{"unknown"};
    bool hardware_accelerated{};
};

struct NativeRenderTarget {
    RenderTargetKind kind{RenderTargetKind::none};
    void* handle{};
    std::uint32_t width{};
    std::uint32_t height{};
    float dpi_scale{1.0F};
};

// Cross-platform subtitle renderer interface.
// Platform backends (D2D, Skia, …) implement this and handle
// window/system integration in their own concrete classes.
class IRenderer {
public:
    virtual ~IRenderer() = default;

    [[nodiscard]] virtual bool initialize() = 0;
    virtual void shutdown() = 0;

    virtual void set_font_family(std::wstring family) = 0;
    [[nodiscard]] virtual const std::wstring& font_family() const noexcept = 0;

    [[nodiscard]] virtual RendererBackendInfo backend_info() const noexcept = 0;

    // Binds the native surface/canvas that subsequent render() calls draw to.
    // The handle type is described by `kind`; unsupported target kinds should
    // return false without side effects.
    [[nodiscard]] virtual bool attach_target(const NativeRenderTarget& target) = 0;

    // Render a subtitle frame at `time` seconds.  The caller owns the
    // render target setup (clear, transform, presentation).
    [[nodiscard]] virtual bool render(const PreparedDocument& document,
        Seconds time, const EngineConfig& config,
        const RenderStyle& style) = 0;
};

} // namespace kirakara::show
