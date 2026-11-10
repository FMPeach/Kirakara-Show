#include "host_hud_renderer.h"
#include "host_hud_svg_icons.h"
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
    HostHudQueueItem current;
    ComPtr<ID2D1SolidColorBrush> brush;
    HostHudSvgIcons icons;
    void* device{};
    std::uint32_t width{}, height{};
    std::uint64_t draws{}, progress_draws{};
    int progress_pixels{-1};
    HostHudFrame frame;
    bool cached{}, ready{}, queue_mode{}, network_wifi{};

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
    const bool has_current = !state->current.title.empty() || !state->current.artist.empty();
    const auto rows = (has_current ? 1U : 0U) + std::max<std::size_t>(1, state->queue.size());
    const float logical_height = queue_mode ? 105.0F + 70.0F * rows : 244.0F;
    const auto card_w = static_cast<std::uint32_t>(std::ceil(500 * scale));
    const auto card_h = static_cast<std::uint32_t>(std::ceil(logical_height * scale));
    const bool resized = p.device != device || p.width != width || p.height != height
        || p.surface.width() != card_w || p.surface.height() != card_h;
    const bool changed = resized || !p.cached || p.queue_mode != queue_mode
        || (queue_mode ? p.queue != state->queue || p.current != state->current
            : p.image != image || p.network != state->network_name
            || p.network_wifi != state->network_wifi);
    p.frame.x = width - std::min(width, card_w + static_cast<std::uint32_t>(std::round(50 * scale)));
    p.frame.y = static_cast<std::uint32_t>(std::round(40 * scale));
    p.frame.width = card_w; p.frame.height = card_h;
    if (changed) {
        p.ready = false;
        if (resized) {
            p.qr.Reset(); p.brush.Reset(); p.icons.reset();
            p.surface = {};
            if (!p.surface.resize_on_device(device, card_w, card_h)) return p.frame;
            p.device = device; p.width = width; p.height = height;
        }
        auto* context = static_cast<ID2D1DeviceContext*>(p.surface.native_render_target());
        const bool image_changed = resized || p.image != image;
        p.queue_mode = queue_mode; p.cached = true; p.progress_pixels = -1;
        if (queue_mode) { p.queue = state->queue; p.current = state->current; }
        else {
            p.image = image; p.network = state->network_name; p.network_wifi = state->network_wifi;
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
            p.text(context, brush.Get(), L"待播队列 · UPCOMING", 16, D2D1::RectF(22, 21, 478, 50), DWRITE_FONT_WEIGHT_BOLD);
            const auto draw_row = [&](const HostHudQueueItem& item, std::size_t row, const std::wstring& marker,
                    bool playing) {
                const auto y = 84.0F + 70.0F * row;
                const auto row_box = D2D1::RoundedRect(D2D1::RectF(22, y, 478, y + 60), 18, 18);
                brush->SetColor(playing ? D2D1::ColorF(245 / 255.0F, 158 / 255.0F, 11 / 255.0F, .12F)
                    : D2D1::ColorF(1, 1, 1, .03F));
                context->FillRoundedRectangle(row_box, brush.Get());
                if (playing) {
                    brush->SetColor(D2D1::ColorF(245 / 255.0F, 158 / 255.0F, 11 / 255.0F, .35F));
                    context->DrawRoundedRectangle(row_box, brush.Get(), 1);
                }
                brush->SetColor(playing ? D2D1::ColorF(245 / 255.0F, 158 / 255.0F, 11 / 255.0F)
                    : D2D1::ColorF(100 / 255.0F, 116 / 255.0F, 139 / 255.0F));
                p.text(context, brush.Get(), marker.c_str(), 15, D2D1::RectF(37, y + 18, 65, y + 45), DWRITE_FONT_WEIGHT_BOLD);
                brush->SetColor(D2D1::ColorF(248 / 255.0F, 250 / 255.0F, 252 / 255.0F));
                p.text(context, brush.Get(), item.title.c_str(), 18, D2D1::RectF(75, y + 5, 460, y + 34), DWRITE_FONT_WEIGHT_BOLD);
                brush->SetColor(D2D1::ColorF(148 / 255.0F, 163 / 255.0F, 184 / 255.0F));
                p.text(context, brush.Get(), item.artist.c_str(), 14, D2D1::RectF(75, y + 33, 460, y + 56));
            };
            if (has_current) draw_row(p.current, 0, L"▶", true);
            for (std::size_t i = 0; i < p.queue.size(); ++i) {
                draw_row(p.queue[i], i + (has_current ? 1 : 0), L"0" + std::to_wstring(i + 1), false);
            }
            if (p.queue.empty()) {
                const auto y = 104.0F + (has_current ? 70.0F : 0.0F);
                brush->SetColor(D2D1::ColorF(148 / 255.0F, 163 / 255.0F, 184 / 255.0F));
                p.text(context, brush.Get(), L"待播队列为空，扫码抢麦", 16, D2D1::RectF(36, y, 478, y + 39));
            }
        } else {
            brush->SetColor(D2D1::ColorF(1, 1, 1));
            context->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(24, 24, 220, 220), 18, 18), brush.Get());
            p.text(context, brush.Get(), L"扫码点歌", 32, D2D1::RectF(244, 44, 480, 88), DWRITE_FONT_WEIGHT_BOLD);
            static_cast<void>(p.icons.draw(context, HostHudIcon::phone, 388, 48, 26));
            brush->SetColor(D2D1::ColorF(148 / 255.0F, 163 / 255.0F, 184 / 255.0F));
            p.text(context, brush.Get(), L"请确保手机和点歌设备", 14, D2D1::RectF(244, 94, 478, 116));
            p.text(context, brush.Get(), L"连接在同一网络下", 14, D2D1::RectF(244, 115, 478, 138));
            if (!p.network.empty()) {
                brush->SetColor(D2D1::ColorF(1, 1, 1, .04F));
                const auto network_box = D2D1::RoundedRect(D2D1::RectF(244, 149, 478, 211), 18, 18);
                context->FillRoundedRectangle(network_box, brush.Get());
                brush->SetColor(D2D1::ColorF(1, 1, 1, .12F));
                context->DrawRoundedRectangle(network_box, brush.Get(), 1);
                static_cast<void>(p.icons.draw(context, p.network_wifi ? HostHudIcon::wifi : HostHudIcon::ethernet,
                    258, 169, 22));
                brush->SetColor(D2D1::ColorF(100 / 255.0F, 116 / 255.0F, 139 / 255.0F));
                p.text(context, brush.Get(), p.network_wifi ? L"当前网络 (Wi-Fi)" : L"当前网络 (有线)",
                    11, D2D1::RectF(291, 158, 464, 176), DWRITE_FONT_WEIGHT_BOLD);
                brush->SetColor(D2D1::ColorF(248 / 255.0F, 250 / 255.0F, 252 / 255.0F));
                p.text(context, brush.Get(), p.network.c_str(), 16, D2D1::RectF(291, 178, 464, 204), DWRITE_FONT_WEIGHT_BOLD);
            }
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
