#include <sdkddkver.h>
#if NTDDI_VERSION < NTDDI_WIN10_RS2
#undef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN10_RS2
#endif
#include "host_hud_svg_icons.h"
#include "hud_svg_data.h"
#include <d2d1_3.h>
#include <d2d1svg.h>
#include <wrl/client.h>
#include <array>
#include <cstring>

using Microsoft::WRL::ComPtr;
struct HostHudSvgIcons::Impl {
    void* owner{};
    ComPtr<ID2D1DeviceContext5> context;
    std::array<ComPtr<ID2D1SvgDocument>, 3> icons;
    std::array<bool, 3> attempted{};
};
HostHudSvgIcons::HostHudSvgIcons() : impl_(std::make_unique<Impl>()) {}
HostHudSvgIcons::~HostHudSvgIcons() = default;
void HostHudSvgIcons::reset() { impl_ = std::make_unique<Impl>(); }
bool HostHudSvgIcons::draw(void* target, HostHudIcon icon, float x, float y, float size) {
    auto* context = static_cast<ID2D1DeviceContext*>(target);
    if (!context) return false;
    if (impl_->owner != target) {
        reset();
        impl_->owner = target;
        static_cast<void>(context->QueryInterface(IID_PPV_ARGS(&impl_->context)));
    }
    auto& p = *impl_;
    if (!p.context) return false;
    const auto index = static_cast<std::size_t>(icon);
    if (!p.attempted[index]) {
        p.attempted[index] = true;
        constexpr std::array sources{kHudWifiSvg, kHudPhoneSvg, kHudEthernetSvg};
        const auto* svg = sources[index];
        ComPtr<IStream> stream;
        ULONG written{};
        LARGE_INTEGER zero{};
        if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))
                && SUCCEEDED(stream->Write(svg, static_cast<ULONG>(std::strlen(svg)), &written))
                && SUCCEEDED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) {
            static_cast<void>(p.context->CreateSvgDocument(stream.Get(), D2D1::SizeF(24, 24), &p.icons[index]));
        }
    }
    if (!p.icons[index]) return false;
    D2D1_MATRIX_3X2_F transform{};
    context->GetTransform(&transform);
    context->SetTransform(D2D1::Matrix3x2F::Scale(size / 24, size / 24)
        * D2D1::Matrix3x2F::Translation(x, y) * transform);
    p.context->DrawSvgDocument(p.icons[index].Get());
    context->SetTransform(transform);
    return true;
}
