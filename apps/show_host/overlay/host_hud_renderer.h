#pragma once
#include "host_hud_state.h"
#include <cstdint>
#include <memory>

struct HostHudFrame {
    void* texture{};
    std::uint32_t x{}, y{}, width{}, height{};
    std::uint64_t revision{};
    bool visible{};
};

// Owner-thread cache of the small card only, shared by the two Host adapters.
class HostHudRenderer {
public:
    HostHudRenderer();
    ~HostHudRenderer();
    const HostHudFrame& render(void* device, std::uint32_t width, std::uint32_t height,
        const HostHudState* state, std::shared_ptr<const HostHudQrImage> image, bool lan_available);
    void reset();
    std::uint64_t rasterizations() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
