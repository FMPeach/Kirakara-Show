// Compile the implementation here to exercise its private layout calculations
// and record actual draw arguments without adding a public/FFI diagnostic API.
// The static library supplies surfaces/resolution; its lyric object is unused.
#define KIRAKARA_SHOW_LAYOUT_TESTS
#include "../src/win32/lyric_renderer.cpp"
#include "kirakara/show/win32/offscreen_surface.hpp"

#include <cstdlib>
#include <iostream>

namespace {

int checks{};
int failures{};

void check(bool value, std::string_view message) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

void expect_near(float actual, float expected, std::string_view message) {
    ++checks;
    if (std::isfinite(actual) && std::abs(actual - expected) <= 0.005F) return;
    ++failures;
    std::cerr << "FAIL: " << message << ": actual=" << actual
        << " expected=" << expected << '\n';
}

void require(bool value, const char* message) {
    if (!value) { std::cerr << "FATAL: " << message << '\n'; std::exit(2); }
}

struct LayoutFixture {
    kirakara::show::win32::OffscreenSurface surface;
    PreviewRenderer renderer;
    kirakara::show::win32::detail::FontResolver reference_resolver;
    std::vector<std::wstring> families;
    IDWriteFontCollection* collection{};

    LayoutFixture() {
        require(surface.resize(1280, 720), "layout test surface");
        require(renderer.initialize(), "layout test renderer");
        require(renderer.attach_external_target(static_cast<ID2D1RenderTarget*>(
            surface.native_render_target())), "attach layout test target");
        require(reference_resolver.initialize(renderer.dwrite), "reference font resolver");
        require(SUCCEEDED(renderer.dwrite->GetSystemFontCollection(&collection)),
            "installed font collection");
    }

    ~LayoutFixture() { release(collection); }

    bool installed(const wchar_t* name) const {
        UINT32 index{}; BOOL exists{};
        return SUCCEEDED(collection->FindFamilyName(name, &index, &exists)) && exists;
    }

    void use(std::vector<std::wstring> names) {
        families = std::move(names); renderer.set_font_families(families);
    }

    float advance(std::string_view text, float size, bool bold) {
        const auto& font = reference_resolver.resolve(families, bold);
        auto* format = reference_resolver.create_format(font, size);
        require(format != nullptr, "reference format");
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        const auto wide = utf8_to_wide(text);
        IDWriteTextLayout* layout{};
        require(SUCCEEDED(renderer.dwrite->CreateTextLayout(wide.c_str(),
            static_cast<UINT32>(wide.size()), format, 4096.0F, size * 1.5F,
            &layout)), "reference layout");
        DWRITE_TEXT_METRICS metrics{};
        require(SUCCEEDED(layout->GetMetrics(&metrics)), "reference metrics");
        release(layout); release(format);
        return std::max(1.0F, metrics.widthIncludingTrailingWhitespace);
    }

    float text_width(std::string_view text, float size, bool bold, float spacing) {
        const auto chars = PreviewRenderer::utf8_chars(text);
        float result{};
        for (const auto& ch : chars) result += advance(ch, size, bold);
        if (chars.size() > 1) result += (chars.size() - 1) * spacing;
        return result;
    }

    void begin() {
        renderer.layout_draws.clear();
        renderer.target->SetTransform(D2D1::Matrix3x2F::Identity());
        renderer.target->BeginDraw();
        renderer.target->Clear(D2D1::ColorF(0, 0, 0, 0));
    }

    void end() { require(SUCCEEDED(renderer.target->EndDraw()), "layout frame draw"); }

    void line(const LyricLine& value, const RenderStyle& style, std::size_t slot) {
        EngineConfig config; config.indicator.enabled = false;
        LineState state; state.opacity = 1;
        begin(); renderer.draw_line(value, state, slot, 2.5, style, config); end();
    }
};

LyricLine plain_line(std::string_view text) {
    LyricLine line;
    for (const auto& ch : PreviewRenderer::utf8_chars(text)) {
        LyricChar item; item.text = ch; item.start_time = 0; item.end_time = 10;
        line.chars.push_back(std::move(item));
    }
    return line;
}

void main_layout_tests(LayoutFixture& fixture) {
    struct FontCase { const wchar_t* request; const wchar_t* installed_family; };
    const FontCase fonts[]{
        {L"Arial", L"Arial"}, {L"Segoe UI", L"Segoe UI"},
        {L"Noto Sans JP Regular", L"Noto Sans JP"},
        {L"Noto Sans JP Medium", L"Noto Sans JP"},
        {L"Noto Sans JP Heavy", L"Noto Sans JP"},
        {L"MotoyaLMaru W3 Mono", L"MotoyaLMaru W3 Mono"},
    };
    const std::vector<std::string> samples{
        wide_to_utf8(L"\u6c38A\u304b\u306aWm09"), "MWmWi109",
        wide_to_utf8(L"\u6c38\u56fd\u6f22\u3042\u30a2"), "MWmA09 ",
    };
    int exercised{};
    bool proportional_difference{};
    for (const auto& font : fonts) {
        if (!fixture.installed(font.installed_family)) {
            std::wcout << L"SKIP: " << font.request << L" (font not installed)\n";
            continue;
        }
        ++exercised;
        fixture.use({font.request, L"sans-serif"});
        RenderStyle style; style.ruby_isolate = false; style.role_colors = false;
        style.stroke_width = 0; style.letter_spacing = 7.25F;
        style.line2_right = 89.0F;
        for (const bool bold : {false, true, false, true}) {
            style.font_bold = bold;
            for (const auto& text : samples) {
                const auto line = plain_line(text);
                const auto expected = fixture.text_width(text, style.font_size,
                    bold, style.letter_spacing);
                for (const auto& ch : line.chars) {
                    expect_near(fixture.renderer.measure(ch.text, style.font_size, bold),
                        fixture.advance(ch.text, style.font_size, bold),
                        "character advance matches the resolved DirectWrite layout");
                }
                expect_near(fixture.renderer.group_base_width(line, 0, line.chars.size(), style),
                    expected, "main group advance uses drawing weight");
                expect_near(fixture.renderer.line_width(line, style), expected,
                    "main line width uses drawing weight");
                for (const std::size_t slot : {0U, 1U}) {
                    fixture.line(line, style, slot);
                    const auto& draws = fixture.renderer.layout_draws;
                    check(draws.size() == line.chars.size(), "one masked draw per main character");
                    const float start = slot == 0 ? style.line1_x
                        : 1280.0F - style.line2_right - expected;
                    float cursor = start;
                    for (std::size_t i = 0; i < std::min(draws.size(), line.chars.size()); ++i) {
                        const auto width = fixture.advance(line.chars[i].text, style.font_size, bold);
                        check(draws[i].bold == bold, "draw request preserves main Bold");
                        expect_near(draws[i].x, cursor, "main drawing cursor / right alignment");
                        expect_near(draws[i].width, width, "main drawing advance matches DirectWrite");
                        cursor += width;
                        if (i + 1 < line.chars.size()) cursor += style.letter_spacing;
                    }
                    expect_near(cursor, start + expected, "last main advance reaches the expected line edge");
                }
            }
        }
        if (std::wstring_view{font.request} == L"Arial"
                || std::wstring_view{font.request} == L"Segoe UI") {
            const auto normal = fixture.text_width("MWmWi109", 62, false, 0);
            const auto bold = fixture.text_width("MWmWi109", 62, true, 0);
            proportional_difference |= std::abs(normal - bold) > 0.01F;
        }
    }
    check(exercised > 0, "at least one installed font exercises main layout");
    check(proportional_difference, "a real proportional Bold case exposes the old width mismatch");
}

} // namespace

int main() {
    require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM initialization");
    {
        LayoutFixture fixture;
        main_layout_tests(fixture);
    }
    CoUninitialize();
    std::cout << "Lyric layout metrics: " << checks << " checks, " << failures << " failures.\n";
    return failures == 0 ? 0 : 1;
}
