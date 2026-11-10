#include "idle_background.h"

#include "kirakara/show/win32/stage_compositor.hpp"

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace {

using Microsoft::WRL::ComPtr;

bool valid_size(std::uint32_t width, std::uint32_t height) noexcept {
    return width > 0 && height > 0
        && width <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
        && height <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
        && static_cast<std::uint64_t>(width) * height * 4U
            <= std::numeric_limits<UINT>::max();
}

std::uint8_t studio_byte(float value, int low, int high) noexcept {
    return static_cast<std::uint8_t>(
        std::clamp(static_cast<int>(std::lround(value)), low, high));
}

} // namespace

bool load_idle_background_bgra(const std::wstring& path,
        std::uint32_t width, std::uint32_t height,
        std::vector<std::uint8_t>& pixels) {
    pixels.clear();
    if (path.empty() || !valid_size(width, height)) return false;

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
            || FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr,
                GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder))
            || FAILED(decoder->GetFrame(0, &frame))) {
        return false;
    }
    UINT source_width{}, source_height{};
    if (FAILED(frame->GetSize(&source_width, &source_height))
            || !valid_size(source_width, source_height)) return false;
    const auto rect = kirakara::show::win32::StageCompositor::letterbox_rect(
        source_width, source_height, {width, height, 60, 1});
    if (FAILED(factory->CreateBitmapScaler(&scaler))
            || FAILED(scaler->Initialize(frame.Get(), rect.width, rect.height,
                WICBitmapInterpolationModeFant))
            || FAILED(factory->CreateFormatConverter(&converter))
            || FAILED(converter->Initialize(scaler.Get(),
                GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return false;
    }

    const auto row_bytes = static_cast<std::size_t>(rect.width) * 4U;
    std::vector<std::uint8_t> scaled(row_bytes * rect.height);
    if (FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(row_bytes),
            static_cast<UINT>(scaled.size()), scaled.data()))) return false;
    pixels.resize(static_cast<std::size_t>(width) * height * 4U, 0);
    for (std::size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    for (std::uint32_t y = 0; y < rect.height; ++y) {
        auto* destination = pixels.data()
            + (static_cast<std::size_t>(rect.y + y) * width + rect.x) * 4U;
        std::copy_n(scaled.data() + y * row_bytes, row_bytes, destination);
        // PBGRA over opaque black: RGB already contains the alpha product.
        for (std::size_t x = 3; x < row_bytes; x += 4) destination[x] = 255;
    }
    return true;
}

bool load_idle_background_nv12(const std::wstring& path,
        std::uint32_t width, std::uint32_t height,
        std::vector<std::uint8_t>& pixels) {
    pixels.clear();
    if ((width & 1U) != 0 || (height & 1U) != 0) return false;
    std::vector<std::uint8_t> bgra;
    if (!load_idle_background_bgra(path, width, height, bgra)) return false;
    const auto area = static_cast<std::size_t>(width) * height;
    pixels.resize(area * 3U / 2U);
    // Match the BT.709 studio-range coefficients used by Cast's compositor.
    for (std::size_t i = 0; i < area; ++i) {
        const auto* p = bgra.data() + i * 4U;
        pixels[i] = studio_byte(16.0F
            + (46.5594F * p[2] + 156.6288F * p[1] + 15.8118F * p[0])
                / 255.0F, 16, 235);
    }
    for (std::uint32_t y = 0; y < height; y += 2) {
        for (std::uint32_t x = 0; x < width; x += 2) {
            float b{}, g{}, r{};
            for (std::uint32_t dy = 0; dy < 2; ++dy) {
                for (std::uint32_t dx = 0; dx < 2; ++dx) {
                    const auto* p = bgra.data()
                        + (static_cast<std::size_t>(y + dy) * width
                            + x + dx) * 4U;
                    b += p[0]; g += p[1]; r += p[2];
                }
            }
            const auto uv = area + static_cast<std::size_t>(y / 2) * width + x;
            pixels[uv] = studio_byte(128.0F
                + (-25.664F * r - 86.336F * g + 112.0F * b)
                    / (4.0F * 255.0F), 16, 240);
            pixels[uv + 1] = studio_byte(128.0F
                + (112.0F * r - 101.730F * g - 10.270F * b)
                    / (4.0F * 255.0F), 16, 240);
        }
    }
    return true;
}

bool IdleBackgroundTexture::configure(ID3D11Device* device,
        std::uint32_t width, std::uint32_t height,
        const IdleBackgroundSource& source) {
    if (device_.Get() == device && width_ == width && height_ == height
            && source_.path == source.path
            && source_.revision == source.revision) {
        return texture_ != nullptr;
    }
    reset();
    if (!device || !valid_size(width, height)) return false;
    device_ = device;
    width_ = width;
    height_ = height;
    source_ = source;
    std::vector<std::uint8_t> pixels;
    if (!load_idle_background_bgra(source.path, width, height, pixels)) return false;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = pixels.data();
    data.SysMemPitch = width * 4U;
    return SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture_));
}

void IdleBackgroundTexture::reset() noexcept {
    texture_.Reset();
    device_.Reset();
    width_ = 0;
    height_ = 0;
    source_ = {};
}
