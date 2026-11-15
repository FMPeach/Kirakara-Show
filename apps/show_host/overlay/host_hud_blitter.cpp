#include "host_hud_blitter.h"
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>

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
namespace {
bool overlaps(const HostHudFrame& a, const HostHudFrame& b) {
    return a.visible && b.visible
        && a.x < b.x + b.width && b.x < a.x + a.width
        && a.y < b.y + b.height && b.y < a.y + a.height;
}
}

struct HostHudRetainedBgraLayer::Impl {
    struct Tile {
        HostHudBlitter blitter;
        ComPtr<ID2D1Bitmap1> underlay;
        HostHudFrame previous;
    };
    ComPtr<ID2D1DeviceContext> context;
    std::array<Tile, 2> tiles;
    std::uint64_t captures{};
};
HostHudRetainedBgraLayer::HostHudRetainedBgraLayer() : impl_(std::make_unique<Impl>()) {}
HostHudRetainedBgraLayer::~HostHudRetainedBgraLayer() = default;
void HostHudRetainedBgraLayer::reset() { impl_ = std::make_unique<Impl>(); }
std::uint64_t HostHudRetainedBgraLayer::captures() const noexcept { return impl_->captures; }
bool HostHudRetainedBgraLayer::apply(void* target, const HostHudLayers& layers,
        bool clean_changed, bool& changed) {
    changed = false;
    auto* context = static_cast<ID2D1DeviceContext*>(target);
    if (!context) return false;
    auto& p = *impl_;
    if (p.context.Get() != context) {
        p.context = context;
        for (auto& tile : p.tiles) {
            tile.underlay.Reset(); tile.previous = {}; tile.blitter.reset();
        }
    }
    const std::array old{p.tiles[0].previous, p.tiles[1].previous};
    const std::array next{layers.card, layers.action};
    std::array dirty{
        clean_changed || !old[0].same_pixels(next[0]),
        clean_changed || !old[1].same_pixels(next[1]),
    };
    if (!dirty[0] && !dirty[1]) return true;
    if (overlaps(old[0], old[1]) || overlaps(next[0], next[1])
            || overlaps(old[0], next[1]) || overlaps(next[0], old[1])) {
        dirty.fill(true);
    }

    // Clear every affected old tile before taking either new underlay. The
    // taller list may move into the old action position (and vice versa).
    if (!clean_changed && ((dirty[0] && old[0].visible) || (dirty[1] && old[1].visible))) {
        D2D1_MATRIX_3X2_F transform{}; context->GetTransform(&transform);
        const auto blend = context->GetPrimitiveBlend();
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
        context->BeginDraw();
        for (std::size_t i = 0; i < old.size(); ++i) {
            const auto& card = old[i];
            if (dirty[i] && card.visible && p.tiles[i].underlay) {
                context->DrawBitmap(p.tiles[i].underlay.Get(),
                    D2D1::RectF(static_cast<float>(card.x), static_cast<float>(card.y),
                        static_cast<float>(card.x + card.width), static_cast<float>(card.y + card.height)),
                    1, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            }
        }
        const auto hr = context->EndDraw();
        context->SetPrimitiveBlend(blend); context->SetTransform(transform);
        if (FAILED(hr)) return false;
        changed = true;
    }
    for (std::size_t i = 0; i < next.size(); ++i) {
        if (dirty[i]) p.tiles[i].previous = {};
    }
    for (std::size_t i = 0; i < next.size(); ++i) {
        const auto& card = next[i];
        if (!dirty[i] || !card.visible) continue;
        auto& underlay = p.tiles[i].underlay;
        const bool resized = !underlay || underlay->GetPixelSize().width != card.width
            || underlay->GetPixelSize().height != card.height;
        if (resized) {
            underlay.Reset();
            const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(context->CreateBitmap(D2D1::SizeU(card.width, card.height), nullptr, 0,
                    &properties, &underlay))) return false;
        }
        if (clean_changed || resized || !old[i].visible || !old[i].same_bounds(card)) {
            const auto source = D2D1::RectU(card.x, card.y, card.x + card.width, card.y + card.height);
            if (FAILED(underlay->CopyFromRenderTarget(nullptr, context, &source))) return false;
            ++p.captures;
        }
    }
    for (std::size_t i = 0; i < next.size(); ++i) {
        if (!dirty[i] || !next[i].visible) continue;
        if (!p.tiles[i].blitter.draw(context, next[i])) return false;
        p.tiles[i].previous = next[i];
        changed = true;
    }
    return true;
}
