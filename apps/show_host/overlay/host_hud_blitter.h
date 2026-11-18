#pragma once
#include "host_hud_renderer.h"
#include <memory>

// Imports the cached card into the destination D2D resource domain. No readback.
class HostHudBlitter {
public:
    HostHudBlitter();
    ~HostHudBlitter();
    bool draw(void* context, const HostHudFrame& card);
    void reset();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Cast C retains only pixels under the HUD tiles, so a paused/held frame can
// scroll, move or remove them without rebuilding the program frame.
class HostHudRetainedBgraLayer {
public:
    HostHudRetainedBgraLayer();
    ~HostHudRetainedBgraLayer();
    bool apply(void* context, const HostHudLayers& layers, bool clean_frame_changed, bool& changed);
    void reset();
    std::uint64_t captures() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
