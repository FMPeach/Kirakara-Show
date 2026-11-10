#pragma once
#include <cstdint>
#include <memory>
#include <string_view>

// Events are coalesced into the Host window; callbacks never enumerate adapters.
class LanStateMonitor {
public:
    LanStateMonitor();
    ~LanStateMonitor();
    void start(void* window, std::uint32_t message);
    void stop() noexcept;
    bool refresh();
    static bool usable_ipv4(std::uint32_t host_order) noexcept;
    static bool usable_adapter(std::wstring_view name, std::uint32_t type, bool up);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
