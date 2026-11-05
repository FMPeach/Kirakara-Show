// Exercise the private character/token caches without adding public test hooks.
#include "../src/win32/lyric_renderer.cpp"
#include "../src/win32/synthetic_bold.hpp"
#include "kirakara/show/win32/offscreen_surface.hpp"

#include <cstdlib>
#include <iostream>

namespace {
int checks{};
void expect(bool value, const char* message) {
    ++checks;
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
void close_to(float a, float b, const char* message) {
    expect(std::abs(a - b) < 0.0001F, message);
}
void same_rect(const D2D1_RECT_F& a, const D2D1_RECT_F& b, const char* message) {
    close_to(a.left, b.left, message); close_to(a.top, b.top, message);
    close_to(a.right, b.right, message); close_to(a.bottom, b.bottom, message);
}
bool installed(PreviewRenderer& renderer, const wchar_t* name) {
    IDWriteFontCollection* fonts{};
    expect(SUCCEEDED(renderer.dwrite->GetSystemFontCollection(&fonts)), "system fonts");
    UINT32 index{}; BOOL exists{};
    fonts->FindFamilyName(name, &index, &exists); fonts->Release();
    return exists;
}
std::uint64_t coverage(const std::vector<std::uint8_t>& pixels) {
    std::uint64_t sum{};
    for (std::size_t i = 3; i < pixels.size(); i += 4) sum += pixels[i];
    return sum;
}

struct Fixture {
    kirakara::show::win32::OffscreenSurface surface;
    PreviewRenderer renderer;
    ID2D1RenderTarget* target{};
    Fixture() {
        expect(surface.resize(512, 256), "bitmap target");
        target = static_cast<ID2D1RenderTarget*>(surface.native_render_target());
        expect(renderer.initialize() && renderer.attach_external_target(target), "bitmap renderer");
    }
    std::vector<std::uint8_t> mask(const PreviewRenderer::CachedGlyphBmp& bmp,
        bool stroke) {
        target->BeginDraw(); target->Clear(D2D1::ColorF(0, 0, 0, 0));
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        auto* white = renderer.brush(D2D1::ColorF(1, 1, 1, 1));
        const auto rect = D2D1::RectF(64 + bmp.ox, 64 + bmp.oy,
            64 + bmp.ox + bmp.sz.width, 64 + bmp.oy + bmp.sz.height);
        target->FillOpacityMask(stroke ? bmp.stroke : bmp.fill, white,
            D2D1_OPACITY_MASK_CONTENT_GRAPHICS, &rect, nullptr);
        expect(SUCCEEDED(target->EndDraw()), "read cached mask");
        return surface.read_rgba8();
    }
    std::vector<std::uint8_t> reference_fill(const CachedTextGeometry& geometry,
        const PreviewRenderer::CachedGlyphBmp& bmp) {
        ID2D1BitmapRenderTarget* reference{};
        const auto pixel_size = bmp.fill->GetPixelSize();
        const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1_ALPHA_MODE_PREMULTIPLIED);
        expect(SUCCEEDED(target->CreateCompatibleRenderTarget(&bmp.sz,
            &pixel_size, &format, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE,
            &reference)), "reference mask target");
        reference->BeginDraw(); reference->Clear(D2D1::ColorF(0, 0, 0, 0));
        reference->SetTransform(D2D1::Matrix3x2F::Translation(-bmp.ox, -bmp.oy));
        ID2D1SolidColorBrush* white{};
        expect(SUCCEEDED(reference->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white)),
            "reference mask brush");
        for (const auto& part : geometry.parts) {
            reference->FillGeometry(part.geometry, white);
            if (part.synthetic_stroke > 0)
                reference->DrawGeometry(part.geometry, white,
                    part.synthetic_stroke, renderer.round_stroke_style);
        }
        expect(SUCCEEDED(reference->EndDraw()), "reference mask draw");
        auto image = bmp;
        expect(SUCCEEDED(reference->GetBitmap(&image.fill)), "reference bitmap");
        auto pixels = mask(image, false);
        image.fill->Release(); white->Release(); reference->Release();
        return pixels;
    }
};

void check_bitmap_bounds(const PreviewRenderer::CachedGlyphBmp& bmp,
    const D2D1_RECT_F& ink, float sw) {
    expect(bmp.fill && bmp.stroke, "both masks exist");
    const auto pixels = bmp.fill->GetPixelSize();
    close_to(static_cast<float>(pixels.width), bmp.sz.width * 3, "3x bitmap width");
    close_to(static_cast<float>(pixels.height), bmp.sz.height * 3, "3x bitmap height");
    // The cached geometry bounds already include half the synthetic pen width.
    // The existing KTV radius and AA padding are added just once here.
    expect(bmp.ox <= ink.left - sw - 1.99F
        && bmp.oy <= ink.top - sw - 1.99F
        && bmp.ox + bmp.sz.width >= ink.right + sw + 1.99F
        && bmp.oy + bmp.sz.height >= ink.bottom + sw + 1.99F,
        "bitmap contains synthetic ink, full KTV radius and AA padding");
    const auto pad = sw > 0 ? sw + 1 : 1;
    same_rect(bmp.reveal, D2D1::RectF(ink.left - pad, ink.top - pad,
        ink.right + pad, ink.bottom + pad), "reveal covers the expanded ink");
}

void check_geometry_bounds(const CachedTextGeometry& geometry, bool eligible, float size) {
    expect(!geometry.parts.empty() && geometry.has_bounds, "cached glyph geometry");
    D2D1_RECT_F expected{};
    bool first = true;
    for (const auto& part : geometry.parts) {
        const auto stroke = eligible
            ? kirakara::show::win32::detail::small_text_skia_stroke(size) : 0.0F;
        close_to(part.synthetic_stroke, stroke, "geometry retains logical synthetic pen width");
        expect(!part.ink || stroke > 0, "only eligible small runs cache a separate ink silhouette");
        D2D1_RECT_F bounds{};
        expect(SUCCEEDED(part.geometry->GetBounds(nullptr, &bounds)), "base path bounds");
        const auto radius = part.synthetic_stroke * 0.5F;
        bounds.left -= radius; bounds.top -= radius;
        bounds.right += radius; bounds.bottom += radius;
        if (part.ink) {
            D2D1_RECT_F ink{};
            expect(SUCCEEDED(part.ink->GetBounds(nullptr, &ink)), "cached ink bounds");
            constexpr float tolerance = 2 * D2D1_DEFAULT_FLATTENING_TOLERANCE
                / PreviewRenderer::glyph_bitmap_supersample;
            expect(ink.left >= bounds.left - tolerance && ink.top >= bounds.top - tolerance
                && ink.right <= bounds.right + tolerance && ink.bottom <= bounds.bottom + tolerance,
                "cached ink stays inside its conservative bitmap envelope");
            float area{}, base_area{};
            expect(SUCCEEDED(part.ink->ComputeArea(nullptr, &area))
                && SUCCEEDED(part.geometry->ComputeArea(nullptr, &base_area)) && area > base_area,
                "cached ink includes the synthetic expansion");
        }
        if (first) { expected = bounds; first = false; }
        else {
            expected.left = std::min(expected.left, bounds.left);
            expected.top = std::min(expected.top, bounds.top);
            expected.right = std::max(expected.right, bounds.right);
            expected.bottom = std::max(expected.bottom, bounds.bottom);
        }
    }
    same_rect(expected, geometry.bounds, "geometry ink expands by half the synthetic stroke");
}

void character_and_token_test(Fixture& fixture, const wchar_t* family, bool eligible) {
    auto& renderer = fixture.renderer;
    renderer.set_font_family(family);
    for (const float size : {22.0F, 26.0F, 28.0F, 29.0F, 30.0F, 40.0F, 62.0F}) {
        for (const auto* text : {"m", "i", ".", "j", "ryoku",
                "\xe3\x81\xaa\xe3\x81\xbf\xe3\x81\xa0"}) {
            for (const bool bold : {false, true}) {
                const auto advance = renderer.measure(text, size, bold);
                const auto* geometry = renderer.text_geometry(text, size, bold);
                expect(geometry != nullptr, "character geometry");
                check_geometry_bounds(*geometry, eligible && bold, size);
                for (const float sw : {0.0F, 3.0F, 3.5F, 12.0F}) {
                    const auto* bmp = renderer.get_glyph_bmp(text, size, bold, sw);
                    expect(bmp != nullptr, "character bitmap");
                    check_bitmap_bounds(*bmp, geometry->bounds, sw);
                    expect(coverage(fixture.mask(*bmp, false)) > 0, "nonempty fill mask");
                    const auto strokes = coverage(fixture.mask(*bmp, true));
                    expect(sw > 0 ? strokes > 0 : strokes == 0, "zero KTV stroke stays disabled");
                    expect(renderer.get_glyph_bmp(text, size, bold, sw) == bmp,
                        "character bitmap cache identity is stable");
                }
                close_to(advance, renderer.measure(text, size, bold), "ink generation preserves advance");
                if (eligible && bold && size < 30) {
                    const auto* bmp = renderer.get_glyph_bmp(text, size, bold, 3);
                    expect(fixture.mask(*bmp, false) == fixture.reference_fill(*geometry, *bmp),
                        "individual glyph masks retain their exact stroke-and-fill rasterization");
                }
            }
            const auto* base = renderer.get_glyph_bmp(text, size, false, 0);
            const auto normal = fixture.mask(*base, false);
            const auto* heavy = renderer.get_glyph_bmp(text, size, true, 0);
            const auto bold_pixels = fixture.mask(*heavy, false);
            if (eligible) expect(coverage(bold_pixels) > coverage(normal), "synthetic fill increases ink coverage");
            // Arial Bold is an explicit intrinsically bold face; its two states
            // must produce exactly the same cached bitmap pixels.
            if (std::wstring_view{family} == L"Arial Bold")
                expect(normal == bold_pixels, "real Bold receives no extra synthetic coverage");
        }
        const std::vector<std::string> chars{"r", "y", "o", "k", "u"};
        for (const bool bold : {false, true}) {
            std::vector<float> widths;
            D2D1_RECT_F ink{};
            float dx{};
            for (std::size_t i = 0; i < chars.size(); ++i) {
                const auto* geometry = renderer.text_geometry(chars[i], size, bold);
                expect(geometry != nullptr, "lower-ruby character geometry");
                const auto& bounds = geometry->bounds;
                if (i == 0) ink = bounds;
                else {
                    ink.left = std::min(ink.left, dx + bounds.left);
                    ink.top = std::min(ink.top, bounds.top);
                    ink.right = std::max(ink.right, dx + bounds.right);
                    ink.bottom = std::max(ink.bottom, bounds.bottom);
                }
                widths.push_back(renderer.measure(chars[i], size, bold));
                dx += widths.back() + (i + 1 < chars.size() ? 2 : 0);
            }
            for (const float sw : {0.0F, 3.0F, 3.5F}) {
                const auto* token = renderer.get_token_bmp(chars, widths, 2, size, bold, sw);
                expect(token != nullptr, "lower-ruby token bitmap");
                check_bitmap_bounds(*token, ink, sw);
                expect(coverage(fixture.mask(*token, false)) > 0, "nonempty lower-ruby fill");
                expect(renderer.get_token_bmp(chars, widths, 2, size, bold, sw) == token,
                    "token bitmap cache identity is stable");
                float advance{};
                for (const auto& ch : chars) advance += renderer.measure(ch, size, bold);
                close_to(dx, advance + 8, "token advance stays tied to layout widths and letter spacing");
            }
        }
    }
}

void ruby_and_cache_test(Fixture& fixture) {
    auto& renderer = fixture.renderer;
    renderer.set_font_family(L"Arial Regular");
    RubyTrack track; track.text = "ryoku";
    DemoStyle style; style.ruby_size = 26; style.ruby2_size = 22;
    style.ruby_bold = style.ruby2_bold = false;
    const auto upper = renderer.measure_track(track, style, false);
    const auto lower = renderer.measure_track(track, style, true);
    style.ruby_bold = style.ruby2_bold = true;
    renderer.get_glyph_bmp("ryoku", 26, true, 3);
    const auto chars = PreviewRenderer::utf8_chars(track.text);
    std::vector<float> widths;
    for (const auto& ch : chars) widths.push_back(renderer.measure(ch, 22, true));
    renderer.get_token_bmp(chars, widths, style.ruby2_letter_spacing, 22, true, 3);
    close_to(upper, renderer.measure_track(track, style, false), "upper-ruby width remains unchanged");
    close_to(lower, renderer.measure_track(track, style, true), "lower-ruby width remains unchanged");
    close_to((180 - upper) * 0.5F, (180 - renderer.measure_track(track, style, false)) * 0.5F,
        "upper-ruby center remains unchanged");
    close_to((180 - lower) * 0.5F, (180 - renderer.measure_track(track, style, true)) * 0.5F,
        "lower-ruby center remains unchanged");
    const auto layouts = renderer.text_layout_cache.size();
    const auto geometries = renderer.text_geometry_cache.size();
    const auto bitmaps = renderer.glyph_bmp_cache.size();
    for (int frame = 0; frame < 100; ++frame) {
        renderer.get_glyph_bmp("ryoku", 26, true, 3);
        renderer.get_token_bmp(chars, widths, style.ruby2_letter_spacing, 22, true, 3);
    }
    expect(layouts == renderer.text_layout_cache.size()
        && geometries == renderer.text_geometry_cache.size()
        && bitmaps == renderer.glyph_bmp_cache.size(), "warm frames create no new layouts, geometry or bitmaps");

    std::vector<ID2D1Geometry*> ink;
    for (const auto& ch : chars) {
        const auto* geometry = renderer.text_geometry(ch, 22, true);
        expect(geometry->parts.size() == 1 && geometry->parts[0].ink, "lower-ruby cached letter ink");
        ink.push_back(geometry->parts[0].ink);
    }
    for (const auto* text : {"uyork", "koryu", "ryuko", "kyour", "rokuy"}) {
        const auto token_chars = PreviewRenderer::utf8_chars(text);
        std::vector<float> token_widths;
        for (const auto& ch : token_chars) token_widths.push_back(renderer.measure(ch, 22, true));
        for (const float sw : {3.0F, 3.5F}) expect(renderer.get_token_bmp(
            token_chars, token_widths, 4, 22, true, sw) != nullptr, "new word from cached letter ink");
        for (std::size_t i = 0; i < chars.size(); ++i)
            expect(renderer.text_geometry(chars[i], 22, true)->parts[0].ink == ink[i],
                "new tokens and before/after masks reuse the same synthetic letter outline");
    }
    expect(layouts == renderer.text_layout_cache.size()
        && geometries == renderer.text_geometry_cache.size(),
        "new words made from cached letters create no new layouts or ink geometry");

    const std::vector<std::string> spaced_chars{"r", " ", "y"};
    std::vector<float> spaced_widths;
    for (const auto& ch : spaced_chars) spaced_widths.push_back(renderer.measure(ch, 22, true));
    const auto* spaced = renderer.get_token_bmp(spaced_chars, spaced_widths, 4, 22, true, 3);
    expect(spaced && coverage(fixture.mask(*spaced, false)) > 0,
        "empty space outlines preserve the surrounding token glyphs");
}
} // namespace

int main() {
    expect(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM");
    {
        Fixture fixture;
        character_and_token_test(fixture, L"Arial Regular", true);
        character_and_token_test(fixture, L"Arial Bold", false);
        if (installed(fixture.renderer, L"Noto Sans JP"))
            character_and_token_test(fixture, L"Noto Sans JP Medium", true);
        ruby_and_cache_test(fixture);
    }
    CoUninitialize();
    std::cout << "Small-text bitmap tests: " << checks << " checks passed.\n";
}
