#include "../apps/show_host/cast/cast_overlay_renderer.h"
#include "../apps/show_host/cast/cast_pipeline_diagnostics.h"

#include "kirakara/show/timeline.hpp"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

using namespace kirakara::show;

namespace {

template <typename T>
void release(T*& value) noexcept {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool create_device(ID3D11Device** device, ID3D11DeviceContext** context) {
    constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL level{};
    auto result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
        nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
        device, &level, context);
    if (SUCCEEDED(result)) return true;
    return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
        nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
        device, &level, context));
}

std::vector<std::uint8_t> read_bgra(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* texture) {
    if (!device || !context || !texture) return {};
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    ID3D11Texture2D* staging{};
    if (FAILED(device->CreateTexture2D(
            &staging_desc, nullptr, &staging))) {
        return {};
    }
    context->CopyResource(staging, texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(
            staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        release(staging);
        return {};
    }
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(desc.Width) * desc.Height * 4U);
    for (std::uint32_t y = 0; y < desc.Height; ++y) {
        const auto* source = static_cast<const std::uint8_t*>(mapped.pData)
            + static_cast<std::size_t>(y) * mapped.RowPitch;
        auto* destination = pixels.data()
            + static_cast<std::size_t>(y) * desc.Width * 4U;
        std::copy_n(source,
            static_cast<std::size_t>(desc.Width) * 4U, destination);
    }
    context->Unmap(staging, 0);
    release(staging);
    return pixels;
}

std::size_t visible_pixels(std::span<const std::uint8_t> bgra) {
    std::size_t count{};
    for (std::size_t index = 3; index < bgra.size(); index += 4) {
        if (bgra[index] != 0) ++count;
    }
    return count;
}

void expect_visible_inside(
        std::span<const std::uint8_t> bgra,
        std::uint32_t width,
        std::uint32_t height,
        const CastOverlayRect& rect) {
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto alpha = bgra[(static_cast<std::size_t>(y) * width + x)
                * 4U + 3U];
            if (alpha == 0) continue;
            expect(x >= rect.x && x < rect.x + rect.width
                    && y >= rect.y && y < rect.y + rect.height,
                "visible overlay pixel is covered by dirty rect");
        }
    }
}

void update_pixel(
        ID3D11DeviceContext* context,
        ID3D11Texture2D* texture,
        std::uint32_t x,
        std::uint32_t y,
        const std::uint8_t (&bgra)[4]) {
    const D3D11_BOX box{x, y, 0, x + 1, y + 1, 1};
    context->UpdateSubresource(texture, 0, &box, bgra, 4, 4);
}

std::size_t first_visible_pixel(std::span<const std::uint8_t> bgra) {
    for (std::size_t index = 3; index < bgra.size(); index += 4) {
        if (bgra[index] != 0) return index / 4;
    }
    return bgra.size() / 4;
}

PreparedDocument make_document(const EngineConfig& engine) {
    LyricChar character;
    character.text = "A";
    character.start_time = 1.0;
    character.end_time = 2.0;
    character.ruby_span = 1;
    LyricLine line;
    line.chars.push_back(std::move(character));
    line.start_time = 1.0;
    line.end_time = 2.0;
    const std::vector<LyricLine> lines{std::move(line)};
    return prepare_timeline(lines, engine);
}

}  // namespace

int main() {
    expect(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)),
        "COM initialization");

    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    expect(create_device(&device, &context), "D3D11 device creation");

    win32::LyricRenderer lyric_renderer;
    win32::SongTitleRenderer title_renderer;
    expect(lyric_renderer.initialize(), "lyric renderer initialization");
    expect(title_renderer.initialize(), "title renderer initialization");

    CastPipelineDiagnostics diagnostics;
    CastOverlayRenderer overlay;
    expect(overlay.configure(device, 320, 180, &diagnostics),
        "overlay configuration");
    expect(overlay.frame().texture != nullptr,
        "overlay exposes a texture");
    expect(overlay.frame().empty, "new overlay starts empty");

    EngineConfig engine;
    engine.indicator.enabled = false;
    engine.fade_mode = FadeMode::disabled;
    RenderStyle style;
    const PreparedDocument empty_document;
    SongTitleConfig empty_title;
    empty_title.enabled = true;
    empty_title.duration = 3.0;
    empty_title.text_fade = false;

    expect(overlay.render(empty_document, empty_title, 0.5,
        engine, style, 1, lyric_renderer, &title_renderer),
        "enabled-empty title is accepted");
    expect(overlay.frame().empty && !overlay.frame().changed,
        "enabled-empty title remains an idle overlay");
    expect(diagnostics.snapshot().event(
        CastPipelineEvent::overlay_draw).count == 0,
        "enabled-empty title performs no D2D draw");

    const auto document = make_document(engine);
    expect(overlay.render(document, empty_title, 1.25,
        engine, style, 2, lyric_renderer, &title_renderer),
        "walking lyric overlay render");
    expect(!overlay.frame().empty && overlay.frame().changed
            && overlay.frame().lyrics_drawn,
        "walking lyric marks overlay changed");
    expect(!overlay.frame().dirty_rect.empty()
            && overlay.frame().dirty_rect.height < overlay.frame().height,
        "lyric update uses a bounded vertical dirty band");
    const auto first_dirty = overlay.frame().dirty_rect;
    const auto first_version = overlay.frame().content_version;
    auto pixels = read_bgra(device, context,
        static_cast<ID3D11Texture2D*>(overlay.frame().texture));
    expect(pixels.size() == 320U * 180U * 4U,
        "overlay texture readback size");
    expect(visible_pixels(pixels) > 10,
        "walking lyric writes non-transparent pixels");
    expect(pixels[3] == 0, "overlay background stays transparent");
    expect_visible_inside(pixels, 320, 180, overlay.frame().dirty_rect);
    const auto old_ink_pixel = first_visible_pixel(pixels);
    expect(old_ink_pixel < 320U * 180U,
        "walking lyric has a sample ink pixel");
    const auto first_draws = diagnostics.snapshot().event(
        CastPipelineEvent::overlay_draw).count;
    expect(first_draws == 1, "first lyric state draws once");

    expect(overlay.render(document, empty_title, 1.25,
        engine, style, 2, lyric_renderer, &title_renderer),
        "unchanged lyric state render");
    expect(!overlay.frame().changed
            && overlay.frame().content_version == first_version,
        "unchanged lyric state reuses cached overlay");
    expect(diagnostics.snapshot().event(
        CastPipelineEvent::overlay_draw).count == first_draws,
        "unchanged lyric state performs no D2D draw");

    const auto sentinel_y = first_dirty.y > 0
        ? 0U : first_dirty.y + first_dirty.height < 180U ? 179U : 180U;
    expect(sentinel_y < 180U,
        "lyric dirty band leaves a sentinel row outside");
    auto* overlay_texture = static_cast<ID3D11Texture2D*>(
        overlay.frame().texture);
    const std::uint8_t sentinel[4]{7, 19, 31, 255};
    update_pixel(context, overlay_texture, 319, sentinel_y, sentinel);

    expect(overlay.render(document, empty_title, 1.5,
        engine, style, 2, lyric_renderer, &title_renderer),
        "advanced walking lyric render");
    expect(overlay.frame().changed
            && overlay.frame().content_version == first_version + 1,
        "walk progress advances overlay version");
    pixels = read_bgra(device, context, overlay_texture);
    const auto sentinel_index =
        (static_cast<std::size_t>(sentinel_y) * 320U + 319U) * 4U;
    expect(pixels[sentinel_index] == sentinel[0]
            && pixels[sentinel_index + 1] == sentinel[1]
            && pixels[sentinel_index + 2] == sentinel[2]
            && pixels[sentinel_index + 3] == sentinel[3],
        "dirty clear preserves pixels outside its band");
    const std::uint8_t transparent[4]{};
    update_pixel(context, overlay_texture, 319, sentinel_y, transparent);

    auto moved_style = style;
    moved_style.line1_y = 100.0F;
    expect(overlay.render(document, empty_title, 1.5,
        engine, moved_style, 3, lyric_renderer, &title_renderer),
        "moved lyric overlay render");
    pixels = read_bgra(device, context, overlay_texture);
    expect(pixels[old_ink_pixel * 4U + 3U] == 0,
        "dirty union erases ink from the previous band");
    expect_visible_inside(pixels, 320, 180, overlay.frame().dirty_rect);

    expect(overlay.render(document, empty_title, 4.1,
        engine, moved_style, 3, lyric_renderer, &title_renderer),
        "lyric exit clears overlay");
    expect(overlay.frame().empty && overlay.frame().changed,
        "lyric exit publishes one transparent update");
    pixels = read_bgra(device, context,
        static_cast<ID3D11Texture2D*>(overlay.frame().texture));
    expect(visible_pixels(pixels) == 0,
        "lyric exit erases all previous pixels");

    SongTitleConfig title;
    title.enabled = true;
    title.duration = 3.0;
    title.text_fade = false;
    SongTitleBlock title_block;
    title_block.lines = {"TITLE"};
    title_block.style.font_family = L"Arial";
    title_block.style.font_families = {L"Arial"};
    title.blocks.push_back(std::move(title_block));
    expect(overlay.render(empty_document, title, 0.5,
        engine, style, 3, lyric_renderer, &title_renderer),
        "title-only overlay render");
    expect(!overlay.frame().empty && overlay.frame().title_drawn
            && !overlay.frame().lyrics_drawn,
        "title-only state is reported");
    expect(!overlay.frame().dirty_rect.empty()
            && overlay.frame().dirty_rect.height < overlay.frame().height,
        "title update uses a bounded vertical dirty band");
    pixels = read_bgra(device, context,
        static_cast<ID3D11Texture2D*>(overlay.frame().texture));
    expect(visible_pixels(pixels) > 100,
        "title writes non-transparent pixels");
    expect_visible_inside(pixels, 320, 180, overlay.frame().dirty_rect);

    overlay.reset();
    title_renderer.shutdown();
    lyric_renderer.shutdown();
    release(context);
    release(device);
    CoUninitialize();
    std::cout << "Cast overlay renderer tests passed.\n";
}
