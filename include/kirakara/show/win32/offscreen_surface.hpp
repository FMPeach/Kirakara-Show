#pragma once

#include "kirakara/show/types.hpp"

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

namespace kirakara::show::win32 {

// Windows reference render target backed by a WIC 32bpp premultiplied BGRA
// bitmap. native_render_target() is an ID2D1RenderTarget* exposed as void* so
// the public header does not force Windows/Direct2D headers on all consumers.
class OffscreenSurface {
public:
    OffscreenSurface();
    ~OffscreenSurface();

    OffscreenSurface(const OffscreenSurface&) = delete;
    OffscreenSurface& operator=(const OffscreenSurface&) = delete;
    OffscreenSurface(OffscreenSurface&&) noexcept;
    OffscreenSurface& operator=(OffscreenSurface&&) noexcept;

    [[nodiscard]] bool resize(std::uint32_t width, std::uint32_t height);
    void begin_draw();
    [[nodiscard]] bool end_draw();
    void clear(Color color);

    [[nodiscard]] void* native_render_target() noexcept;
    [[nodiscard]] const void* native_render_target() const noexcept;
    [[nodiscard]] bool read_rgba8_into(std::uint8_t* buffer,
        std::size_t buffer_size) const;
    [[nodiscard]] std::vector<std::uint8_t> read_rgba8() const;
    [[nodiscard]] bool save_png(std::wstring_view path) const;

    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kirakara::show::win32
