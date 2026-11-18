#include "host_announcement_renderer.h"
#include "announcement_scroll.h"
#include "kirakara/show/win32/d3d11_texture_surface.hpp"

#include <d2d1_1.h>
#include <dwrite_1.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cwctype>

using Microsoft::WRL::ComPtr;

struct HostAnnouncementRenderer::Impl {
    kirakara::show::win32::D3D11TextureSurface surface;
    ComPtr<IDWriteFactory> fonts;
    ComPtr<IDWriteTextFormat> format;
    ComPtr<IDWriteTextLayout> layout;
    ComPtr<ID2D1LinearGradientBrush> background, ink;
    ComPtr<ID2D1SolidColorBrush> border;
    std::wstring text;
    AnnouncementScroll scroll;
    HostHudFrame frame;
    void* device{};
    float scale{};
    double measured_width{};
    float text_y{};
    std::uint64_t prepared_submission{};
    bool prepared{}, blank{};
    std::uint64_t layout_count{}, draw_count{};

    bool measure(const std::wstring& value) {
        if (layout && text == value) return true;
        if (!fonts && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(fonts.GetAddressOf())))) return false;
        if (!format && (FAILED(fonts->CreateTextFormat(L"Segoe UI", nullptr,
                DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 26, L"zh-CN", &format))
                || FAILED(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)))) return false;
        ComPtr<IDWriteTextLayout> next;
        if (FAILED(fonts->CreateTextLayout(value.data(), static_cast<UINT32>(value.size()),
                format.Get(), static_cast<float>(kHostAnnouncementMaxTextUnits * 52), 72, &next))) return false;
        ComPtr<IDWriteTextLayout1> spaced;
        if (FAILED(next.As(&spaced)) || FAILED(spaced->SetCharacterSpacing(
                .26F, .26F, 0, {0, static_cast<UINT32>(value.size())}))) return false;
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(next->GetMetrics(&metrics))) return false;
        measured_width = metrics.widthIncludingTrailingWhitespace;
        text_y = (72-metrics.height)*.5F;
        text = value;
        layout = std::move(next);
        ++layout_count;
        return true;
    }

    bool target(void* requested, std::uint32_t w, std::uint32_t h, float s) {
        if (device == requested && surface.width() == w && surface.height() == h && scale == s) return true;
        background.Reset(); ink.Reset(); border.Reset();
        device = nullptr;
        surface = {};
        if (!surface.resize_on_device(requested, w, h)) return false;
        auto* context = static_cast<ID2D1DeviceContext*>(surface.native_render_target());
        const float logical_width = w / s;
        const D2D1_GRADIENT_STOP bg[]{{0, {14/255.F, 18/255.F, 26/255.F, .70F}},
            {1, {14/255.F, 18/255.F, 26/255.F, .54F}}};
        ComPtr<ID2D1GradientStopCollection> stops;
        if (FAILED(context->CreateGradientStopCollection(bg, 2, &stops))
                || FAILED(context->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties({0,0}, {0,72}), stops.Get(), &background))) return false;
        stops.Reset();
        const auto color = [](float alpha) { return D2D1::ColorF(226/255.F,232/255.F,240/255.F,alpha); };
        const D2D1_GRADIENT_STOP fade[]{{0,color(0)},
            {24/logical_width,color(0)}, {112/logical_width,color(1)},
            {std::max(112.F,logical_width-156)/logical_width,color(1)},
            {(logical_width-24)/logical_width,color(0)}, {1,color(0)}};
        if (FAILED(context->CreateGradientStopCollection(fade, 6, &stops))
                || FAILED(context->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties({0,0}, {logical_width,0}), stops.Get(), &ink))
                || FAILED(context->CreateSolidColorBrush(D2D1::ColorF(1,1,1,.12F), &border))) return false;
        device = requested;
        scale = s;
        scroll.hide();
        return true;
    }
};

HostAnnouncementRenderer::HostAnnouncementRenderer() : impl_(std::make_unique<Impl>()) {}
HostAnnouncementRenderer::~HostAnnouncementRenderer() = default;
void HostAnnouncementRenderer::reset() { impl_ = std::make_unique<Impl>(); }
void HostAnnouncementRenderer::hide() noexcept {
    impl_->scroll.hide();
    impl_->frame.visible = impl_->frame.animating = false;
}
std::uint64_t HostAnnouncementRenderer::layouts() const noexcept { return impl_->layout_count; }
std::uint64_t HostAnnouncementRenderer::rasterizations() const noexcept { return impl_->draw_count; }
double HostAnnouncementRenderer::text_width() const noexcept { return impl_->measured_width; }
bool HostAnnouncementRenderer::visible() const noexcept { return impl_->frame.visible; }

const HostHudFrame& HostAnnouncementRenderer::render(void* device, std::uint32_t width,
        std::uint32_t height, const HostAnnouncementText* value, bool enabled,
        const HostHudFrame& card, double now) {
    auto& p = *impl_;
    p.frame.visible = p.frame.animating = p.frame.layout_ready = false;
    if (!device || !width || !height || !enabled || !value || !value->visible
            || value->text.empty() || value->text.size() > kHostAnnouncementMaxTextUnits
            || card.scene == HostHudScene::queue || card.target_scene == HostHudScene::queue
            || !std::isfinite(now)) {
        p.scroll.hide();
        return p.frame;
    }
    if (!p.prepared || p.prepared_submission != value->submission) {
        p.blank = std::all_of(value->text.begin(), value->text.end(),
            [](wchar_t c) { return std::iswspace(c) != 0; });
        if (!p.blank && !p.measure(value->text)) return p.frame;
        p.prepared_submission = value->submission;
        p.prepared = true;
    }
    if (p.blank) {
        p.scroll.hide();
        return p.frame;
    }
    const float scale = host_hud_scale(width, height);
    const auto left = static_cast<std::uint32_t>(std::round(50 * scale));
    const auto right = card.layout_ready && card.scene == HostHudScene::qr
        ? card.x - std::min(card.x, static_cast<std::uint32_t>(std::round(40 * scale)))
        : width - std::min(width, left);
    if (right <= left || scale <= 0 || (right-left)/scale < 272) {
        p.scroll.hide();
        return p.frame;
    }
    if (!p.target(device, right-left,
            static_cast<std::uint32_t>(std::ceil(72*scale)), scale)) return p.frame;
    const auto copies = p.scroll.sample(value->submission, (right-left)/scale, p.measured_width, now);
    if (!copies.count) return p.frame;
    auto* context = static_cast<ID2D1DeviceContext*>(p.surface.native_render_target());
    context->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    p.surface.begin_draw();
    p.surface.clear({0,0,0,0});
    const float logical_width = (right-left)/scale;
    const auto rect = D2D1::RoundedRect(D2D1::RectF(.5F,.5F,logical_width-.5F,71.5F),18,18);
    context->FillRoundedRectangle(rect, p.background.Get());
    context->DrawRoundedRectangle(rect, p.border.Get(), 1);
    context->PushAxisAlignedClip(D2D1::RectF(0,0,logical_width,72),D2D1_ANTIALIAS_MODE_ALIASED);
    for (std::size_t i=0;i<copies.count;++i) {
        const auto x = copies.x(i);
        if (x >= logical_width || x+p.measured_width <= 0) continue;
        context->DrawTextLayout({static_cast<float>(x),p.text_y},p.layout.Get(),p.ink.Get());
    }
    context->PopAxisAlignedClip();
    if (!p.surface.end_draw()) return p.frame;
    p.surface.flush_d2d();
    p.frame.texture=p.surface.native_texture();
    p.frame.x=left; p.frame.y=static_cast<std::uint32_t>(std::round(40*scale));
    p.frame.width=right-left; p.frame.height=p.surface.height();
    p.frame.revision=++p.draw_count;
    p.frame.visible=p.frame.layout_ready=p.frame.animating=true;
    return p.frame;
}
