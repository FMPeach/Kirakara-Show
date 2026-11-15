#include "../apps/show_host/cast/cast_overlay_renderer.h"
#include "../apps/show_host/cast/cast_pipeline_diagnostics.h"
#include "../apps/show_host/overlay/host_hud_renderer.h"

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

    const auto first_title_pixel = first_visible_pixel(pixels);
    auto next_title = title;
    next_title.blocks.front().style.y = 440.0F;
    // Match the one-time refresh_cast_render_configuration() load handoff.
    title_renderer.invalidate_block_cache();
    expect(overlay.render(empty_document, next_title, 0.5,
        engine, style, 4, lyric_renderer, &title_renderer),
        "next Cast song with repeated credit text");
    pixels = read_bgra(device, context,
        static_cast<ID3D11Texture2D*>(overlay.frame().texture));
    expect(visible_pixels(pixels) > 100,
        "next Cast title remains visible");
    expect(first_visible_pixel(pixels) / 320U
            > first_title_pixel / 320U + 50U,
        "next Cast song must use its own title Y instead of cached placement");
    expect(pixels[first_title_pixel * 4U + 3U] == 0,
        "changing Cast title layout erases the previous song's ink");
    expect_visible_inside(pixels, 320, 180, overlay.frame().dirty_rect);

    const auto title_version = overlay.frame().content_version;
    const auto title_draws = diagnostics.snapshot().event(
        CastPipelineEvent::overlay_draw).count;
    expect(overlay.render(empty_document, next_title, 1.0,
        engine, style, 4, lyric_renderer, &title_renderer),
        "unchanged next-song title");
    expect(!overlay.frame().changed
            && overlay.frame().content_version == title_version
            && diagnostics.snapshot().event(
                CastPipelineEvent::overlay_draw).count == title_draws,
        "unchanged title must not redraw or flush caches every frame");

    {
        kirakara::show::win32::D3D11TextureSurface card;
        expect(card.resize_on_device(device, 100, 30), "HUD texture");
        card.begin_draw(); card.clear({1, 0, 0, .8F});
        expect(card.end_draw(), "HUD fill"); card.flush_d2d();
        HostHudLayers hud;
        hud.card = {card.native_texture(), 200, 4, 100, 30, 1, true};
        auto low_style = style; low_style.line1_y = 600;
        expect(overlay.render(document, empty_title, 1.5, engine, low_style, 5,
            lyric_renderer, &title_renderer, &hud), "lyrics plus top HUD");
        expect(overlay.frame().coverage_count == 2, "HUD and lyric coverage stay separate");
        const auto version = overlay.frame().content_version;
        expect(overlay.render(document, empty_title, 1.5, engine, low_style, 5,
            lyric_renderer, &title_renderer, &hud) && overlay.frame().content_version == version,
            "static combined overlay reuses texture");
        const auto before_fade = read_bgra(device, context, static_cast<ID3D11Texture2D*>(overlay.frame().texture));
        hud.card.opacity = .5F;
        expect(overlay.render(document, empty_title, 1.5, engine, low_style, 5,
            lyric_renderer, &title_renderer, &hud) && overlay.frame().content_version == version + 1,
            "opacity alone invalidates the small Cast HUD region");
        expect(overlay.frame().dirty_rect.x == 200 && overlay.frame().dirty_rect.width == 100
            && overlay.frame().dirty_rect.y == 4 && overlay.frame().dirty_rect.height == 30,
            "card-only fade leaves the disjoint lyric band out of the dirty region");
        pixels = read_bgra(device, context, static_cast<ID3D11Texture2D*>(overlay.frame().texture));
        expect(std::equal(pixels.begin() + 60 * 320 * 4, pixels.end(),
            before_fade.begin() + 60 * 320 * 4), "HUD fade preserves all lower program pixels");
        expect(pixels[(10 * 320 + 250) * 4 + 3] >= 99
            && pixels[(10 * 320 + 250) * 4 + 3] <= 105, "Cast A keeps half of cached HUD alpha");
        expect(overlay.render(document, empty_title, 1.5, engine, low_style, 5,
            lyric_renderer, &title_renderer, &hud) && !overlay.frame().changed,
            "stable partial opacity is still cached");
        hud.card.x = 0; hud.card.y = 130; hud.card.opacity = .75F;
        expect(overlay.render(document, empty_title, 1.5, engine, low_style, 5,
            lyric_renderer, &title_renderer, &hud), "fade over a potentially overlapping lyric band");
        hud.card.visible = false;
        expect(overlay.render(document, empty_title, 1.5, engine, low_style, 5,
            lyric_renderer, &title_renderer, &hud), "HUD removed during held program");
        pixels = read_bgra(device, context, static_cast<ID3D11Texture2D*>(overlay.frame().texture));
        expect(pixels[(10 * 320 + 250) * 4 + 3] == 0, "old HUD is erased");
        expect(std::equal(pixels.begin() + 60 * 320 * 4, pixels.end(),
            before_fade.begin() + 60 * 320 * 4), "overlapping HUD removal restores the unchanged lyric pixels");
        expect(overlay.frame().coverage_count == 1, "lyric-only ROI restored");

        const auto clean = pixels;
        HostHudActionRenderer actions;
        hud.card = {card.native_texture(), 200, 4, 100, 30, 1, true, 1, false, true};
        hud.action = actions.render(device, 320, 180, hud.card, {HostHudAction::play, 1});
        expect(hud.action.visible && hud.action.y == 38, "Cast A action under the card");
        expect(overlay.render(document, empty_title, 1.5, engine, low_style, 5,
            lyric_renderer, &title_renderer, &hud), "add action independently");
        expect(overlay.frame().coverage_count == 3, "main HUD, action and lyrics have separate ROIs");
        const auto first = read_bgra(device, context, static_cast<ID3D11Texture2D*>(overlay.frame().texture));
        hud.action.opacity = .5F;
        expect(overlay.render(document, empty_title, 1.5, engine, low_style, 5,
            lyric_renderer, &title_renderer, &hud), "fade action only");
        const auto faded = read_bgra(device, context, static_cast<ID3D11Texture2D*>(overlay.frame().texture));
        const auto dirty = overlay.frame().dirty_rect;
        expect(dirty.x >= hud.action.x - 1 && dirty.y >= hud.action.y - 1
            && dirty.y + dirty.height <= hud.action.y + hud.action.height + 1,
            "only the action's even-aligned rectangle is dirty");
        expect(!std::equal(faded.begin(), faded.end(), first.begin()), "action actually fades");
        expect(std::equal(faded.begin(), faded.begin() + 34 * 320 * 4, first.begin()),
            "action fade leaves main HUD pixels unchanged");
        const auto old_center = ((hud.action.y + hud.action.height / 2) * 320
            + hud.action.x + hud.action.width / 2) * 4;
        hud.action.y = 64;
        expect(overlay.render(document, empty_title, 1.5, engine, low_style, 5,
            lyric_renderer, &title_renderer, &hud), "move action below a taller HUD");
        pixels = read_bgra(device, context, static_cast<ID3D11Texture2D*>(overlay.frame().texture));
        expect(pixels[old_center + 3] == 0 && first[old_center + 3] > 0, "moving clears old action pixels");
        // If a custom subtitle band meets the tile, its UV-aligned region
        // must not be dispatched a second time.
        const auto band = overlay.frame().coverage_regions[0];
        hud.action.y = band.y > 4 ? band.y - 4 : band.y;
        expect(overlay.render(document, empty_title, 1.5, engine, low_style, 5,
                lyric_renderer, &title_renderer, &hud), "action can straddle the subtitle band");
        const auto& coverage = overlay.frame();
        for (std::uint32_t i = 0; i < coverage.coverage_count; ++i) {
            const auto a = coverage.coverage_regions[i];
            for (std::uint32_t j = i + 1; j < coverage.coverage_count; ++j) {
                const auto b = coverage.coverage_regions[j];
                expect(a.x + a.width <= b.x || b.x + b.width <= a.x
                    || a.y + a.height <= b.y || b.y + b.height <= a.y,
                    "NV12 ROI dispatches never overlap");
            }
        }
        hud = {};
        expect(overlay.render(document, empty_title, 1.5, engine, low_style, 5,
            lyric_renderer, &title_renderer, &hud), "remove both HUD tiles");
        expect(read_bgra(device, context, static_cast<ID3D11Texture2D*>(overlay.frame().texture)) == clean,
            "Cast A restores exactly the program overlay");
    }
    overlay.reset();
    title_renderer.shutdown();
    lyric_renderer.shutdown();
    release(context);
    release(device);
    CoUninitialize();
    std::cout << "Cast overlay renderer tests passed.\n";
}
