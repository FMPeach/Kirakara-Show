#pragma once

#include <cstdint>
#include <memory>

struct HostHudLayers;

// Host-owned small composition layer above the existing Stage HWND output.
// Both cached tiles are positioned in a single DComp transaction.
class PhysicalHudOutput {
public:
    PhysicalHudOutput();
    ~PhysicalHudOutput();
    PhysicalHudOutput(const PhysicalHudOutput&) = delete;
    PhysicalHudOutput& operator=(const PhysicalHudOutput&) = delete;

    [[nodiscard]] bool initialize(void* hwnd, void* d3d_device);
    [[nodiscard]] bool present(const HostHudLayers& layers);
    void shutdown() noexcept;
    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] std::uint64_t submissions() const noexcept;
    [[nodiscard]] std::int32_t last_error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
