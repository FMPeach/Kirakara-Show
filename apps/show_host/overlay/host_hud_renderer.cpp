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
    std::vector<HostHudQueueItem> queue;
    ComPtr<ID2D1SolidColorBrush> brush;
    void* device{};
    std::uint32_t width{}, height{};
    std::uint64_t draws{}, progress_draws{};
    int progress_pixels{-1};
    HostHudFrame frame;
    bool cached{}, ready{}, queue_mode{};

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
std::uint64_t HostHudRenderer::progress_updates() const noexcept { return impl_->progress_draws; }
const HostHudFrame& HostHudRenderer::render(void* device, std::uint32_t width,
        std::uint32_t height, const HostHudState* state,
        std::shared_ptr<const HostHudQrImage> image, bool lan_available, HostHudPresentation presentation) {
    auto& p = *impl_;
    p.frame.visible = false;
    if (!device || !width || !height || !state) return p.frame;
    const bool queue_mode = presentation.queue_visible && state->options.queue_enabled;
    if (!queue_mode && (!state->options.qr_enabled || !lan_available || !image || !image->valid())) return p.frame;
    const float scale = std::min(width / 1920.0F, height / 1080.0F);
    const float logical_height = queue_mode ? 105.0F + 70.0F * std::max<std::size_t>(1, state->queue.size()) : 244.0F;
    const auto card_w = static_cast<std::uint32_t>(std::ceil(500 * scale));
    const auto card_h = static_cast<std::uint32_t>(std::ceil(logical_height * scale));
    const bool resized = p.device != device || p.width != width || p.height != height
        || p.surface.width() != card_w || p.surface.height() != card_h;
    const bool changed = resized || !p.cached || p.queue_mode != queue_mode
        || (queue_mode ? p.queue != state->queue : p.image != image || p.network != state->network_name);
    p.frame.x = width - std::min(width, card_w + static_cast<std::uint32_t>(std::round(50 * scale)));
    p.frame.y = static_cast<std::uint32_t>(std::round(40 * scale));
    p.frame.width = card_w; p.frame.height = card_h;
    if (changed) {
        p.ready = false;
        if (resized) {
            p.qr.Reset(); p.brush.Reset();
            p.surface = {};
            if (!p.surface.resize_on_device(device, card_w, card_h)) return p.frame;
            p.device = device; p.width = width; p.height = height;
        }
        auto* context = static_cast<ID2D1DeviceContext*>(p.surface.native_render_target());
        const bool image_changed = resized || p.image != image;
        p.queue_mode = queue_mode; p.cached = true; p.progress_pixels = -1;
        if (queue_mode) p.queue = state->queue;
        else {
            p.image = image; p.network = state->network_name;
            if (image_changed && !p.load_qr(context, *image)) return p.frame;
            if (!p.qr || 180 * scale < image->modules) return p.frame;
        }
        if (!p.fonts && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(p.fonts.GetAddressOf())))) return p.frame;
        if (!p.brush && FAILED(context->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &p.brush))) return p.frame;
        auto& brush = p.brush;
        p.surface.begin_draw(); p.surface.clear({0, 0, 0, 0});
        context->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
        brush->SetColor(D2D1::ColorF(14 / 255.0F, 18 / 255.0F, 26 / 255.0F, .94F));
        const auto box = D2D1::RoundedRect(D2D1::RectF(1, 1, 499, logical_height - 1), 25, 25);
        context->FillRoundedRectangle(box, brush.Get());
        brush->SetColor(D2D1::ColorF(1, 1, 1, .12F));
        context->DrawRoundedRectangle(box, brush.Get(), 1);
        if (queue_mode) {
            brush->SetColor(D2D1::ColorF(245 / 255.0F, 158 / 255.0F, 11 / 255.0F));
            p.text(context, brush.Get(), L"待播列表", 18, D2D1::RectF(22, 21, 478, 50), DWRITE_FONT_WEIGHT_BOLD);
            if (p.queue.empty()) {
                brush->SetColor(D2D1::ColorF(148 / 255.0F, 163 / 255.0F, 184 / 255.0F));
                p.text(context, brush.Get(), L"快来点一首喜欢的歌吧", 16, D2D1::RectF(36, 104, 478, 143));
            }
            for (std::size_t i = 0; i < p.queue.size(); ++i) {
                const auto y = 84.0F + 70.0F * i;
                brush->SetColor(i == 0 ? D2D1::ColorF(245 / 255.0F, 158 / 255.0F, 11 / 255.0F, .12F)
                    : D2D1::ColorF(1, 1, 1, .03F));
                context->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(22, y, 478, y + 60), 18, 18), brush.Get());
                brush->SetColor(i == 0 ? D2D1::ColorF(245 / 255.0F, 158 / 255.0F, 11 / 255.0F)
                    : D2D1::ColorF(100 / 255.0F, 116 / 255.0F, 139 / 255.0F));
                const auto number = std::to_wstring(i + 1);
                p.text(context, brush.Get(), number.c_str(), 16, D2D1::RectF(37, y + 18, 65, y + 45), DWRITE_FONT_WEIGHT_BOLD);
                brush->SetColor(D2D1::ColorF(248 / 255.0F, 250 / 255.0F, 252 / 255.0F));
                p.text(context, brush.Get(), p.queue[i].title.c_str(), 16, D2D1::RectF(75, y + 7, 460, y + 33), DWRITE_FONT_WEIGHT_BOLD);
                brush->SetColor(D2D1::ColorF(148 / 255.0F, 163 / 255.0F, 184 / 255.0F));
                p.text(context, brush.Get(), p.queue[i].artist.c_str(), 12, D2D1::RectF(75, y + 34, 460, y + 55));
            }
        } else {
            brush->SetColor(D2D1::ColorF(1, 1, 1));
            context->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(24, 24, 220, 220), 18, 18), brush.Get());
            p.text(context, brush.Get(), L"扫码点歌", 32, D2D1::RectF(244, 44, 480, 88), DWRITE_FONT_WEIGHT_BOLD);
            brush->SetColor(D2D1::ColorF(148 / 255.0F, 163 / 255.0F, 184 / 255.0F));
            p.text(context, brush.Get(), L"手机连接同一局域网", 14, D2D1::RectF(244, 98, 478, 126));
            p.text(context, brush.Get(), L"点歌地址", 11, D2D1::RectF(244, 149, 478, 167));
            brush->SetColor(D2D1::ColorF(6 / 255.0F, 182 / 255.0F, 212 / 255.0F));
            p.text(context, brush.Get(), p.network.c_str(), 16, D2D1::RectF(244, 171, 478, 201), DWRITE_FONT_WEIGHT_BOLD);
        }
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        if (!queue_mode) {
            const auto module_pixels = std::max(1U, static_cast<std::uint32_t>(180 * scale) / image->modules);
            const auto side = static_cast<float>(module_pixels * image->modules);
            const auto left = std::round(122 * scale - side / 2);
            const auto top = std::round(122 * scale - side / 2);
            context->DrawBitmap(p.qr.Get(), D2D1::RectF(left, top, left + side, top + side),
                1, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        }
        if (!p.surface.end_draw()) { p.cached = false; return p.frame; }
        p.surface.flush_d2d();
        ++p.draws; ++p.frame.revision;
        p.ready = true;
    }
    if (queue_mode && p.ready) {
        const auto track_width = std::round(456 * scale);
        const auto pixels = static_cast<int>(std::round(track_width * std::clamp(presentation.progress, 0.0F, 1.0F)));
        if (pixels != p.progress_pixels) {
            auto* context = static_cast<ID2D1DeviceContext*>(p.surface.native_render_target());
            const auto left = std::round(22 * scale), top = std::round(62 * scale);
            const auto bottom = top + std::max(1.0F, std::round(4 * scale));
            p.surface.begin_draw();
            static_cast<void>(p.surface.clear_rect(left, top, left + track_width, bottom,
                {14 / 255.0F, 18 / 255.0F, 26 / 255.0F, .94F}));
            p.brush->SetColor(D2D1::ColorF(1, 1, 1, .08F));
            context->FillRectangle(D2D1::RectF(left, top, left + track_width, bottom), p.brush.Get());
            p.brush->SetColor(D2D1::ColorF(245 / 255.0F, 158 / 255.0F, 11 / 255.0F));
            if (pixels) context->FillRectangle(D2D1::RectF(left, top, left + pixels, bottom), p.brush.Get());
            if (!p.surface.end_draw()) { p.cached = false; return p.frame; }
            p.surface.flush_d2d(); p.progress_pixels = pixels;
            ++p.progress_draws; ++p.frame.revision;
        }
    }
    p.frame.texture = p.surface.native_texture();
    p.frame.visible = p.ready && p.frame.texture != nullptr;
    return p.frame;
}
