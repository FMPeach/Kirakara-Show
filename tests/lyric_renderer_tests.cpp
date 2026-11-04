#include "kirakara/show/lrc_parser.hpp"
#include "kirakara/show/win32/d3d11_texture_surface.hpp"
#include "kirakara/show/win32/lyric_renderer.hpp"
#include "kirakara/show/win32/offscreen_surface.hpp"
#include "kirakara/show/config.hpp"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace kirakara::show;

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    expect(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)),
        "COM initialization");
    {
        EngineConfig config;
        config.indicator.enabled = false;
        const auto parsed = parse_lrc(
            "【@A+B】[00:01:00]{今日|きょ[00:02:00]う>today}"
            "[00:04:00]は[00:05:00]\n", config);
        expect(parsed.document.lines.size() == 1, "fixture parsing");

        win32::OffscreenSurface surface;
        expect(surface.resize(1280, 720), "offscreen target creation");
        win32::LyricRenderer renderer;
        expect(renderer.initialize(), "renderer initialization");
        const auto backend = renderer.backend_info();
        expect(backend.backend == RendererBackend::direct2d,
            "Direct2D backend identity");
        expect(backend.name == std::string_view{"Direct2D"},
            "Direct2D backend name");
        IRenderer& generic_renderer = renderer;
        expect(!generic_renderer.attach_target(NativeRenderTarget{
            .kind = RenderTargetKind::skia_canvas,
            .handle = surface.native_render_target(),
        }), "unsupported target kind rejected");
        expect(generic_renderer.attach_target(NativeRenderTarget{
            .kind = RenderTargetKind::direct2d_render_target,
            .handle = surface.native_render_target(),
            .width = surface.width(),
            .height = surface.height(),
        }), "generic native target attachment");
        kirakara::show::RenderStyle style;
        expect(style.font_size == 62.0F, "preset font size default");
        expect(style.stroke_width == 4.0F, "preset stroke width default");
        expect(style.letter_spacing == 9.0F, "preset letter spacing default");
        expect(style.ruby_bold, "preset ruby bold default");
        expect(style.ruby_stroke_width == 3.0F, "preset ruby stroke default");
        expect(style.ruby_letter_spacing == 5.0F, "preset ruby letter spacing default");
        expect(style.line1_y == 430.0F, "H5 line1Y default");
        expect(style.line2_y == 566.0F, "preset line2Y default");
        expect(style.ruby_above_offset == 4.0F, "DOM rubyOffset default");
        expect(style.ruby_below_offset == 4.0F, "DOM ruby2Offset default");
        const auto preset = default_app_config();
        expect(preset.style.font_size == 62.0F, "app preset font size");
        expect(preset.style.letter_spacing == 12.0F, "app preset spacing");
        expect(preset.style.ruby_letter_spacing == 5.0F, "app preset ruby spacing");
        expect(!preset.style.ruby_isolate, "app preset ruby isolate off");
        expect(preset.style.ruby_above_offset == 1.0F, "app preset ruby offset");
        expect(preset.style.line1_y == 450.0F, "app preset line1Y");
        expect(preset.style.line2_y == 566.0F, "app preset line2Y");
        expect(preset.engine.fade_duration > 0.665
            && preset.engine.fade_duration < 0.667, "app preset fade duration");
        expect(preset.engine.indicator.enabled, "app preset indicator enabled");
        expect(preset.engine.indicator.size == 34.0F,
            "app preset indicator size");
        expect(preset.font_families.size() == 2,
            "preset font family fallback chain");
        expect(preset.font_families[0] == L"MotoyaLMaru W3 Mono",
            "preset primary font family");
        expect(preset.font_families[1] == L"monospace",
            "preset generic font fallback");
        expect(renderer.font_family() == L"MotoyaLMaru W3 Mono",
            "preset font family default");

        auto font_config = preset;
        expect(load_app_config_json(
            "{\"fontFamily\":\"'Missing Demo Font', sans-serif\"}",
            font_config), "font family config parse");
        expect(font_config.font_families.size() == 2,
            "CSS font family list parse");
        expect(font_config.font_families[0] == L"Missing Demo Font",
            "CSS primary font family parse");
        expect(font_config.font_families[1] == L"sans-serif",
            "CSS generic fallback parse");
        expect(load_app_config_json("{\"fontSize\":66}", font_config),
            "partial config parse");
        expect(font_config.font_families[0] == L"Missing Demo Font",
            "partial config preserves font family chain");

        auto* native_target = static_cast<ID2D1RenderTarget*>(
            surface.native_render_target());
        native_target->BeginDraw();
        native_target->Clear(D2D1::ColorF(0, 0, 0, 0));
        native_target->EndDraw();
        expect(true, "initial transparent clear");

        const auto caller_transform = D2D1::Matrix3x2F::Translation(7.0F, 11.0F);
        native_target->SetTransform(caller_transform);
        expect(renderer.render(parsed.document, 2.5, config, style),
            "public renderer frame");
        D2D1_MATRIX_3X2_F restored_transform{};
        native_target->GetTransform(&restored_transform);
        expect(restored_transform._11 == caller_transform._11
               && restored_transform._12 == caller_transform._12
               && restored_transform._21 == caller_transform._21
               && restored_transform._22 == caller_transform._22
               && restored_transform._31 == caller_transform._31
               && restored_transform._32 == caller_transform._32,
            "renderer restores caller transform");

        const auto pixels = surface.read_rgba8();
        expect(pixels.size() == 1280U * 720U * 4U, "RGBA8 frame size");
        // The engine now renders on a transparent background.  Verify that
        // subtitle glyphs produced visible (non-transparent) pixels.
        std::size_t visible{};
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            if (pixels[i + 3] > 0) ++visible;
        }
        expect(visible > 500, "visible subtitle glyphs on transparent background");

        // Reuse one renderer while changing named weights and each Bold switch.
        // Every cached result must agree with a fresh renderer of the same style.
        // Arial is a Windows fixture; no optional Noto installation is needed.
        const auto cache_fixture = parse_lrc(
            "[00:00:00]{MMMM|mmmm>mmmm}[00:10:00]\n", config);
        expect(cache_fixture.document.lines.size() == 1, "font cache fixture");
        auto cache_style = style;
        cache_style.stroke_width = 0;
        cache_style.ruby_stroke_width = 0;
        cache_style.ruby2_stroke_width = 0;
        cache_style.font_bold = cache_style.ruby_bold = cache_style.ruby2_bold = false;
        auto draw_cached = [&](win32::LyricRenderer& value,
                const std::wstring& family, const RenderStyle& test_style) {
            value.set_font_families({family, L"sans-serif"});
            native_target->SetTransform(D2D1::Matrix3x2F::Identity());
            native_target->BeginDraw();
            native_target->Clear(D2D1::ColorF(0, 0, 0, 0));
            expect(SUCCEEDED(native_target->EndDraw()), "font cache transparent clear");
            expect(value.render_overlay(cache_fixture.document, 2.5, config, test_style),
                "font cache frame");
            return surface.read_rgba8();
        };
        const auto regular_pixels = draw_cached(renderer, L"Arial Regular", cache_style);
        const auto heavy_pixels = draw_cached(renderer, L"Arial Heavy", cache_style);
        expect(regular_pixels != heavy_pixels, "named Regular and Heavy produce different glyphs");
        expect(draw_cached(renderer, L"Arial Regular", cache_style) == regular_pixels,
            "changing back restores Regular without stale Heavy caches");
        const auto medium_normal = draw_cached(renderer, L"Arial Medium", cache_style);
        for (int track = 0; track < 3; ++track) {
            auto bold_style = cache_style;
            if (track == 0) bold_style.font_bold = true;
            if (track == 1) bold_style.ruby_bold = true;
            if (track == 2) bold_style.ruby2_bold = true;
            const auto reused = draw_cached(renderer, L"Arial Medium", bold_style);
            expect(reused != medium_normal, "each track's additional Bold affects its glyphs");
            win32::LyricRenderer fresh;
            expect(fresh.initialize() && fresh.attach_native_target(native_target),
                "fresh font cache reference renderer");
            const auto reference = draw_cached(fresh, L"Arial Medium", bold_style);
            expect(reused == reference, "main/ruby/ruby2 weight caches agree with fresh renderer");
            expect(draw_cached(renderer, L"Arial Regular", cache_style) == regular_pixels,
                "turning Bold back off restores cached normal frame");
        }

        IDWriteFactory* fonts_factory{};
        expect(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&fonts_factory))), "bitmap font factory");
        IDWriteFontCollection* fonts{};
        expect(SUCCEEDED(fonts_factory->GetSystemFontCollection(&fonts)), "bitmap font collection");
        UINT32 noto_index{}; BOOL has_noto{};
        fonts->FindFamilyName(L"Noto Sans JP", &noto_index, &has_noto);
        auto coverage = [](const std::vector<std::uint8_t>& pixels) {
            std::uint64_t total{};
            for (std::size_t i = 3; i < pixels.size(); i += 4) total += pixels[i];
            return total;
        };
        for (const auto* family : {L"Arial Regular", L"Noto Sans JP Medium",
            L"Noto Sans JP SemiBold", L"Noto Sans JP Heavy"}) {
            if (std::wstring_view{family}.starts_with(L"Noto") && !has_noto) continue;
            const bool intrinsic_bold = std::wstring_view{family}.ends_with(L"SemiBold")
                || std::wstring_view{family}.ends_with(L"Heavy");
            const auto normal = draw_cached(renderer, family, cache_style);
            for (int track = 0; track < 3; ++track) {
                auto bold_style = cache_style;
                if (track == 0) bold_style.font_bold = true;
                if (track == 1) bold_style.ruby_bold = true;
                if (track == 2) bold_style.ruby2_bold = true;
                const auto pixels = draw_cached(renderer, family, bold_style);
                if (intrinsic_bold) expect(pixels == normal,
                    "intrinsic SemiBold/Heavy bitmap is unchanged by additional Bold");
                else expect(pixels != normal && coverage(pixels) > coverage(normal),
                    "native draw-only Bold increases main/ruby/ruby2 bitmap coverage");
                expect(draw_cached(renderer, family, cache_style) == normal,
                    "normal bitmap cache survives native Bold on every track");
            }
        }
        fonts->Release(); fonts_factory->Release();

        win32::D3D11TextureSurface gpu_surface;
        expect(gpu_surface.resize(320, 180), "D3D11 texture target creation");
        expect(gpu_surface.native_texture() != nullptr, "native GPU texture handle");
        expect(renderer.attach_native_target(gpu_surface.native_render_target()),
            "D3D11 Direct2D target attachment");
        expect(renderer.render(parsed.document, 2.5, config, style),
            "GPU texture lyric render");
        expect(gpu_surface.synchronize(), "GPU completion fence");
        const auto gpu_pixels = gpu_surface.read_rgba8();
        expect(gpu_pixels.size() == 320U * 180U * 4U,
            "GPU texture diagnostic readback");
        std::size_t gpu_non_background{};
        for (std::size_t i = 0; i < gpu_pixels.size(); i += 4) {
            const bool green = gpu_pixels[i] >= 6 && gpu_pixels[i] <= 12
                && gpu_pixels[i + 1] >= 82 && gpu_pixels[i + 1] <= 88
                && gpu_pixels[i + 2] <= 3;
            if (!green) ++gpu_non_background;
        }
        expect(gpu_non_background > 50, "GPU-rendered glyph pixels");
        renderer.shutdown();
    }
    CoUninitialize();
    std::cout << "Public LyricRenderer API test passed.\n";
}
