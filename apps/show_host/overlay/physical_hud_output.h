#pragma once

#include <cstdint>
#include <memory>

// Host-owned small composition layer above the existing Stage HWND output.
// It consumes a cached BGRA texture and never writes a shared program frame.
class PhysicalHudOutput {
public:
    PhysicalHudOutput();
    ~PhysicalHudOutput();
    PhysicalHudOutput(const PhysicalHudOutput&) = delete;
    PhysicalHudOutput& operator=(const PhysicalHudOutput&) = delete;

    [[nodiscard]] bool initialize(void* hwnd, void* d3d_device);
    [[nodiscard]] bool present(void* texture, float left, float top,
        std::uint64_t content_revision, float opacity = 1.0F);
    [[nodiscard]] bool set_visible(bool visible);
    void shutdown() noexcept;
    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] std::uint64_t submissions() const noexcept;
    [[nodiscard]] std::int32_t last_error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
