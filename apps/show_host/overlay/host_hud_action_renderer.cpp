#include "host_hud_renderer.h"
#include "host_hud_svg_icons.h"
#include "kirakara/show/win32/d3d11_texture_surface.hpp"

#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <string_view>

namespace {
using kirakara::show::win32::D3D11TextureSurface;
using Microsoft::WRL::ComPtr;
constexpr float kSide = 128;
constexpr float kGap = 24;
constexpr struct {
    std::wstring_view label;
    HostHudIcon icon;
} kTiles[] = {
    {L"播放", HostHudIcon::play},
    {L"暂停", HostHudIcon::pause},
    {L"重唱", HostHudIcon::replay},
    {L"切歌", HostHudIcon::skip},
    {L"原唱", HostHudIcon::vocal},
    {L"伴奏", HostHudIcon::accompaniment},
    {L"音量＋", HostHudIcon::volume_up},
    {L"音量－", HostHudIcon::volume_down},
};
static_assert(std::size(kTiles) == static_cast<std::size_t>(HostHudAction::volume_down));
}

struct HostHudActionRenderer::Impl {
    struct Tile {
        D3D11TextureSurface surface;
        bool attempted{}, ready{};
        std::uint64_t revision{};
    };
    std::array<Tile, std::size(kTiles)> tiles;
    void* device{};
    std::uint32_t pixels{};
    std::uint64_t draws{};

    bool draw(Tile& tile, std::size_t index) {
        if (!tile.surface.resize_on_device(device, pixels, pixels)) return false;
        auto* context = static_cast<ID2D1DeviceContext*>(tile.surface.native_render_target());
        ComPtr<ID2D1SolidColorBrush> brush;
        ComPtr<IDWriteFactory> fonts;
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(context->CreateSolidColorBrush(D2D1::ColorF(0x0e121a, .94F), &brush))
                || FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                    __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(fonts.GetAddressOf())))
                || FAILED(fonts->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
                    DWRITE_FONT_WEIGHT_BLACK, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    20, L"zh-CN", &format))) return false;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        tile.surface.begin_draw();
        tile.surface.clear({0, 0, 0, 0});
        const auto scale = pixels / kSide;
        context->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
        const auto box = D2D1::RoundedRect(D2D1::RectF(1, 1, kSide - 1, kSide - 1), 24, 24);
        context->FillRoundedRectangle(box, brush.Get());
        brush->SetColor(D2D1::ColorF(1, 1, 1, .12F));
        context->DrawRoundedRectangle(box, brush.Get(), 1);
        HostHudSvgIcons icons;
        const auto& design = kTiles[index];
        static_cast<void>(icons.draw(context, design.icon, 45, 24, 38));
        brush->SetColor(D2D1::ColorF(0xf8fafc));
        context->DrawText(design.label.data(), static_cast<UINT32>(design.label.size()), format.Get(),
            D2D1::RectF(0, 74, kSide, 106), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (!tile.surface.end_draw()) return false;
        tile.surface.flush_d2d();
        tile.revision = ++draws;
        return true;
    }
};

HostHudActionRenderer::HostHudActionRenderer() : impl_(std::make_unique<Impl>()) {}
HostHudActionRenderer::~HostHudActionRenderer() = default;
void HostHudActionRenderer::reset() { impl_ = std::make_unique<Impl>(); }
std::uint64_t HostHudActionRenderer::rasterizations() const noexcept { return impl_->draws; }

HostHudFrame HostHudActionRenderer::render(void* device, std::uint32_t width,
        std::uint32_t height, const HostHudFrame& card, HostHudActionAppearance action) {
    const auto kind = static_cast<std::uint32_t>(action.action);
    if (!device || !width || !height || kind == 0 || kind > std::size(kTiles)
            || action.opacity <= 0) return {};
    const auto scale = host_hud_scale(width, height);
    const auto pixels = static_cast<std::uint32_t>(std::ceil(kSide * scale));
    const auto right = card.layout_ready ? card.x + card.width
        : width - static_cast<std::uint32_t>(std::round(kHostHudRight * scale));
    const auto y = card.layout_ready ? card.y + card.height
        + static_cast<std::uint32_t>(std::round(kGap * scale))
        : static_cast<std::uint32_t>(std::round(kHostHudTop * scale));
    if (right > width || right < pixels || y > height || pixels > height - y) return {};
    auto& p = *impl_;
    if (p.device != device || p.pixels != pixels) {
        for (auto& tile : p.tiles) tile = {};
        p.device = device;
        p.pixels = pixels;
    }
    auto& tile = p.tiles[kind - 1];
    if (!tile.attempted) {
        tile.attempted = true;
        tile.ready = p.draw(tile, kind - 1);
    }
    if (!tile.ready) return {};
    return {tile.surface.native_texture(), right - pixels, y, pixels, pixels,
        tile.revision, true, action.opacity, action.animating, true};
}
