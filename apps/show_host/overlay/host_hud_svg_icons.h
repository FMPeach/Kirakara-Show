#pragma once
#include <memory>

enum class HostHudIcon { wifi, phone, ethernet };
class HostHudSvgIcons {
public:
    HostHudSvgIcons();
    ~HostHudSvgIcons();
    bool draw(void* context, HostHudIcon icon, float x, float y, float size);
    void reset();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
