#pragma once

#include "host_hud_renderer.h"
#include <memory>
#include <string>

inline constexpr std::size_t kHostAnnouncementMaxTextUnits = 16384;

struct HostAnnouncementText {
    std::uint64_t submission{};
    bool visible{};
    std::wstring text;
};

class HostAnnouncementRenderer {
public:
    HostAnnouncementRenderer();
    ~HostAnnouncementRenderer();
    const HostHudFrame& render(void* device, std::uint32_t width,
        std::uint32_t height, const HostAnnouncementText* text, bool enabled,
        const HostHudFrame& card, double monotonic_seconds);
    void reset();
    void hide() noexcept;
    [[nodiscard]] std::uint64_t layouts() const noexcept;
    [[nodiscard]] std::uint64_t rasterizations() const noexcept;
    [[nodiscard]] double text_width() const noexcept;
    [[nodiscard]] bool visible() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
