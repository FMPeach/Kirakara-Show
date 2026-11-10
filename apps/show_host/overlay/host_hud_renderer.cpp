#include "host_hud_renderer.h"
#include "kirakara/show/win32/d3d11_texture_surface.hpp"
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;
struct HostHudRenderer::Impl {
    kirakara::show::win32::D3D11TextureSurface surface;
    ComPtr<ID2D1Bitmap1> qr;
    ComPtr<IDWriteFactory> fonts;
    std::shared_ptr<const HostHudQrImage> image;
    std::wstring network;
    void* device{};
    std::uint32_t width{}, height{};
    std::uint64_t draws{};
    HostHudFrame frame;
    bool cached{}, ready{};

    bool load_qr(ID2D1DeviceContext* context, const HostHudQrImage& input) {
        qr.Reset();
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> decoded;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory))) || FAILED(factory->CreateStream(&stream))
                || FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(input.png.data()),
                    static_cast<DWORD>(input.png.size())))
                || FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                    WICDecodeMetadataCacheOnLoad, &decoder))) return false;
        GUID container{};
        UINT w{}, h{};
        if (FAILED(decoder->GetContainerFormat(&container)) || container != GUID_ContainerFormatPng
                || FAILED(decoder->GetFrame(0, &decoded)) || FAILED(decoded->GetSize(&w, &h))
                || w != input.pixel_size || h != input.pixel_size
                || FAILED(factory->CreateFormatConverter(&converter))
                || FAILED(converter->Initialize(decoded.Get(), GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
        return SUCCEEDED(context->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &qr));
    }
    void text(ID2D1DeviceContext* context, ID2D1Brush* brush, const wchar_t* value,
            float size, D2D1_RECT_F rect, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL) {
        ComPtr<IDWriteTextFormat> format;
        ComPtr<IDWriteTextLayout> layout;
        if (!fonts || FAILED(fonts->CreateTextFormat(L"Microsoft YaHei UI", nullptr, weight,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &format))) return;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(fonts->CreateEllipsisTrimmingSign(format.Get(), &ellipsis)))
            format->SetTrimming(&trimming, ellipsis.Get());
        if (SUCCEEDED(fonts->CreateTextLayout(value, static_cast<UINT32>(wcslen(value)),
                format.Get(), rect.right - rect.left, rect.bottom - rect.top, &layout)))
            context->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(), brush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
};
HostHudRenderer::HostHudRenderer() : impl_(std::make_unique<Impl>()) {}
HostHudRenderer::~HostHudRenderer() = default;
void HostHudRenderer::reset() { impl_ = std::make_unique<Impl>(); }
std::uint64_t HostHudRenderer::rasterizations() const noexcept { return impl_->draws; }
const HostHudFrame& HostHudRenderer::render(void* device, std::uint32_t width,
        std::uint32_t height, const HostHudState* state,
        std::shared_ptr<const HostHudQrImage> image, bool lan_available) {
    auto& p = *impl_;
    p.frame.visible = false;
    if (!device || !width || !height || !state || !state->options.qr_enabled
            || !lan_available || !image || !image->valid()) return p.frame;
    const bool resized = p.device != device || p.width != width || p.height != height;
    const bool changed = resized || !p.cached || p.image != image || p.network != state->network_name;
    const float scale = std::min(width / 1920.0F, height / 1080.0F);
    const auto card_w = static_cast<std::uint32_t>(std::ceil(500 * scale));
    const auto card_h = static_cast<std::uint32_t>(std::ceil(244 * scale));
    p.frame.x = width - std::min(width, card_w + static_cast<std::uint32_t>(std::round(50 * scale)));
    p.frame.y = static_cast<std::uint32_t>(std::round(40 * scale));
    p.frame.width = card_w; p.frame.height = card_h;
    if (changed) {
        p.ready = false;
        if (resized) {
            p.qr.Reset();
            p.surface = {};
            if (!p.surface.resize_on_device(device, card_w, card_h)) return p.frame;
            p.device = device; p.width = width; p.height = height;
        }
        auto* context = static_cast<ID2D1DeviceContext*>(p.surface.native_render_target());
        const bool image_changed = resized || p.image != image;
        p.image = image; p.network = state->network_name; p.cached = true;
        if (image_changed && !p.load_qr(context, *image)) return p.frame;
        if (!p.qr) return p.frame;
        if (!p.fonts && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(p.fonts.GetAddressOf())))) return p.frame;
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(context->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush))) return p.frame;
        p.surface.begin_draw(); p.surface.clear({0, 0, 0, 0});
        context->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
        brush->SetColor(D2D1::ColorF(14 / 255.0F, 18 / 255.0F, 26 / 255.0F, .94F));
        const auto box = D2D1::RoundedRect(D2D1::RectF(1, 1, 499, 243), 25, 25);
        context->FillRoundedRectangle(box, brush.Get());
        brush->SetColor(D2D1::ColorF(1, 1, 1, .12F));
        context->DrawRoundedRectangle(box, brush.Get(), 1);
        brush->SetColor(D2D1::ColorF(1, 1, 1));
        context->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(24, 24, 220, 220), 18, 18), brush.Get());
        p.text(context, brush.Get(), L"扫码点歌", 32, D2D1::RectF(244, 44, 480, 88), DWRITE_FONT_WEIGHT_BOLD);
        brush->SetColor(D2D1::ColorF(148 / 255.0F, 163 / 255.0F, 184 / 255.0F));
        p.text(context, brush.Get(), L"手机连接同一局域网", 14, D2D1::RectF(244, 98, 478, 126));
        p.text(context, brush.Get(), L"点歌地址", 11, D2D1::RectF(244, 149, 478, 167));
        brush->SetColor(D2D1::ColorF(6 / 255.0F, 182 / 255.0F, 212 / 255.0F));
        p.text(context, brush.Get(), p.network.c_str(), 16, D2D1::RectF(244, 171, 478, 201), DWRITE_FONT_WEIGHT_BOLD);
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        const auto module_pixels = std::max(1U, static_cast<std::uint32_t>(180 * scale) / image->modules);
        const auto side = static_cast<float>(module_pixels * image->modules);
        const auto left = std::round(122 * scale - side / 2);
        const auto top = std::round(122 * scale - side / 2);
        context->DrawBitmap(p.qr.Get(), D2D1::RectF(left, top, left + side, top + side),
            1, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        if (!p.surface.end_draw()) { p.cached = false; return p.frame; }
        p.surface.flush_d2d();
        ++p.draws; ++p.frame.revision;
        p.ready = true;
    }
    p.frame.texture = p.surface.native_texture();
    p.frame.visible = p.ready && p.qr != nullptr && p.frame.texture != nullptr;
    return p.frame;
}
