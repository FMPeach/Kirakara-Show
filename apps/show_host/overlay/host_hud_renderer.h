#pragma once
#include "host_hud_state.h"
#include "host_hud_controller.h"
#include <cstdint>
#include <memory>

struct HostHudFrame {
    void* texture{};
    std::uint32_t x{}, y{}, width{}, height{};
    std::uint64_t revision{};
    bool visible{};
    float opacity{1};
    bool animating{};
    // A ready card at zero opacity still owns its placement during handoff.
    bool layout_ready{};

    [[nodiscard]] bool same_bounds(const HostHudFrame& other) const noexcept {
        return x == other.x && y == other.y && width == other.width && height == other.height;
    }
    [[nodiscard]] bool same_pixels(const HostHudFrame& other) const noexcept {
        return visible == other.visible && (!visible
            || (same_bounds(other) && texture == other.texture
                && revision == other.revision && opacity == other.opacity));
    }
};

inline constexpr float kHostHudTop = 40;
inline constexpr float kHostHudRight = 50;

[[nodiscard]] inline float host_hud_scale(std::uint32_t width,
        std::uint32_t height) noexcept {
    const auto sx = width / 1920.0F;
    const auto sy = height / 1080.0F;
    return sx < sy ? sx : sy;
}

struct HostHudLayers {
    HostHudFrame card;
    HostHudFrame action;
};

// Owner-thread cache of the small card only, shared by the two Host adapters.
class HostHudRenderer {
public:
    HostHudRenderer();
    ~HostHudRenderer();
    const HostHudFrame& render(void* device, std::uint32_t width, std::uint32_t height,
        const HostHudState* state, std::shared_ptr<const HostHudQrImage> image, bool lan_available,
        HostHudPresentation presentation = {});
    void reset();
    std::uint64_t rasterizations() const noexcept;
    std::uint64_t progress_updates() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Immutable action tiles are independent of QR/list texture churn.
class HostHudActionRenderer {
public:
    HostHudActionRenderer();
    ~HostHudActionRenderer();
    [[nodiscard]] HostHudFrame render(void* device, std::uint32_t width,
        std::uint32_t height, const HostHudFrame& card, HostHudActionAppearance action);
    void reset();
    [[nodiscard]] std::uint64_t rasterizations() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
