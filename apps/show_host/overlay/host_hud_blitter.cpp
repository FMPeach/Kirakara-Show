#include "host_hud_blitter.h"
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
struct HostHudBlitter::Impl {
    ComPtr<ID2D1DeviceContext> context;
    ComPtr<ID3D11Texture2D> source;
    ComPtr<ID2D1Bitmap1> bitmap;
};
HostHudBlitter::HostHudBlitter() : impl_(std::make_unique<Impl>()) {}
HostHudBlitter::~HostHudBlitter() = default;
void HostHudBlitter::reset() { impl_ = std::make_unique<Impl>(); }
bool HostHudBlitter::draw(void* target, const HostHudFrame& card) {
    if (!card.visible) return true;
    auto* context = static_cast<ID2D1DeviceContext*>(target);
    auto* texture = static_cast<ID3D11Texture2D*>(card.texture);
    if (!context || !texture) return false;
    auto& p = *impl_;
    if (p.context.Get() != context || p.source.Get() != texture || !p.bitmap) {
        p.bitmap.Reset(); p.context = context; p.source = texture;
        ComPtr<IDXGISurface> surface;
        if (FAILED(texture->QueryInterface(IID_PPV_ARGS(&surface)))
                || FAILED(context->CreateBitmapFromDxgiSurface(surface.Get(), nullptr, &p.bitmap))) return false;
    }
    D2D1_MATRIX_3X2_F transform{}; context->GetTransform(&transform);
    const auto blend = context->GetPrimitiveBlend();
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    context->BeginDraw();
    context->DrawBitmap(p.bitmap.Get(), D2D1::RectF(static_cast<float>(card.x),
        static_cast<float>(card.y), static_cast<float>(card.x + card.width),
        static_cast<float>(card.y + card.height)), card.opacity, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    const auto hr = context->EndDraw();
    context->SetPrimitiveBlend(blend); context->SetTransform(transform);
    return SUCCEEDED(hr);
}
struct HostHudRetainedBgraLayer::Impl {
    HostHudBlitter blitter;
    ComPtr<ID2D1DeviceContext> context;
    ComPtr<ID2D1Bitmap1> underlay;
    HostHudFrame previous;
    std::uint64_t captures{};
};
HostHudRetainedBgraLayer::HostHudRetainedBgraLayer() : impl_(std::make_unique<Impl>()) {}
HostHudRetainedBgraLayer::~HostHudRetainedBgraLayer() = default;
void HostHudRetainedBgraLayer::reset() { impl_ = std::make_unique<Impl>(); }
std::uint64_t HostHudRetainedBgraLayer::captures() const noexcept { return impl_->captures; }
bool HostHudRetainedBgraLayer::apply(void* target, const HostHudFrame& card, bool clean_changed, bool& changed) {
    changed = false;
    auto* context = static_cast<ID2D1DeviceContext*>(target);
    if (!context) return false;
    auto& p = *impl_;
    if (p.context.Get() != context) {
        p.context = context; p.underlay.Reset(); p.previous = {}; p.blitter.reset();
    }
    const auto old = p.previous;
    const bool same = old.visible == card.visible && (!card.visible || (
        old.texture == card.texture && old.revision == card.revision && old.x == card.x
        && old.y == card.y && old.width == card.width && old.height == card.height
        && old.opacity == card.opacity));
    if (!clean_changed && same) return true;
    if (!old.visible && !card.visible) return true;
    if (!clean_changed && old.visible && p.underlay) {
        D2D1_MATRIX_3X2_F transform{}; context->GetTransform(&transform);
        const auto blend = context->GetPrimitiveBlend();
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
        context->BeginDraw();
        context->DrawBitmap(p.underlay.Get(), D2D1::RectF(static_cast<float>(old.x), static_cast<float>(old.y),
            static_cast<float>(old.x + old.width), static_cast<float>(old.y + old.height)),
            1, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        const auto hr = context->EndDraw();
        context->SetPrimitiveBlend(blend); context->SetTransform(transform);
        if (FAILED(hr)) return false;
        changed = true;
    }
    if (card.visible) {
        if (!p.underlay || p.underlay->GetPixelSize().width != card.width
                || p.underlay->GetPixelSize().height != card.height) {
            p.underlay.Reset();
            const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(context->CreateBitmap(D2D1::SizeU(card.width, card.height), nullptr, 0,
                    &properties, &p.underlay))) return false;
        }
        if (clean_changed || !old.visible || old.x != card.x || old.y != card.y
                || old.width != card.width || old.height != card.height) {
            const auto source = D2D1::RectU(card.x, card.y, card.x + card.width, card.y + card.height);
            if (FAILED(p.underlay->CopyFromRenderTarget(nullptr, context, &source))) return false;
            ++p.captures;
        }
        if (!p.blitter.draw(context, card)) return false;
        changed = true;
    }
    p.previous = card;
    return true;
}
