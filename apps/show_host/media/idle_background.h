#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

struct IdleBackgroundSource {
    std::wstring path;
    std::uint64_t revision{};
};

// Images belong to the embedding application. Decode/resize only on source
// or output changes, and cache failed attempts until the next explicit update.
[[nodiscard]] bool load_idle_background_bgra(
    const std::wstring& path, std::uint32_t width, std::uint32_t height,
    std::vector<std::uint8_t>& pixels);

// One-time BT.709 limited-range conversion for Cast A's static NV12 base.
// Odd dimensions are invalid for NV12; BGRA/native outputs support them.
[[nodiscard]] bool load_idle_background_nv12(
    const std::wstring& path, std::uint32_t width, std::uint32_t height,
    std::vector<std::uint8_t>& pixels);

class IdleBackgroundTexture {
public:
    [[nodiscard]] bool configure(
        ID3D11Device* device, std::uint32_t width, std::uint32_t height,
        const IdleBackgroundSource& source);
    void reset() noexcept;

    [[nodiscard]] ID3D11Texture2D* texture() const noexcept {
        return texture_.Get();
    }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    std::uint32_t width_{};
    std::uint32_t height_{};
    IdleBackgroundSource source_;
};
