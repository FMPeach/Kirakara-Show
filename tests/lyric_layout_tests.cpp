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

struct FontCase { const wchar_t* request; const wchar_t* installed_family; };
const FontCase fonts[]{
    {L"Arial", L"Arial"}, {L"Segoe UI", L"Segoe UI"},
    {L"Noto Sans JP Regular", L"Noto Sans JP"},
    {L"Noto Sans JP Medium", L"Noto Sans JP"},
    {L"Noto Sans JP Heavy", L"Noto Sans JP"},
    {L"MotoyaLMaru W3 Mono", L"MotoyaLMaru W3 Mono"},
};

void main_layout_tests(LayoutFixture& fixture) {
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

std::vector<PreviewRenderer::LayoutDraw> draws_of(const LayoutFixture& fixture,
    PreviewRenderer::LayoutKind kind, float size) {
    std::vector<PreviewRenderer::LayoutDraw> result;
    for (const auto& draw : fixture.renderer.layout_draws)
        if (draw.kind == kind && draw.size == size) result.push_back(draw);
    return result;
}

void ruby_track_tests(LayoutFixture& fixture) {
    const std::vector<std::vector<std::string>> samples{
        {"Romaji ", "AWm"},
        {wide_to_utf8(L"\u6c38\u56fd"), wide_to_utf8(L"\u6f22\u3042\u30a2")},
        {wide_to_utf8(L"\u6c38A\u304b"), "Wm09"},
    };
    for (const auto& font : fonts) {
        if (!fixture.installed(font.installed_family)) continue;
        fixture.use({font.request, L"sans-serif"});
        for (int flags = 0; flags < 4; ++flags) {
            RenderStyle style; style.stroke_width = 0; style.role_colors = false;
            style.ruby_bold = (flags & 1) != 0;
            style.ruby2_bold = (flags & 2) != 0;
            style.ruby_letter_spacing = 2.75F; style.ruby2_letter_spacing = 3.25F;
            for (const bool lower : {false, true}) {
                const auto size = lower ? style.ruby2_size : style.ruby_size;
                const auto bold = lower ? style.ruby2_bold : style.ruby_bold;
                const auto spacing = lower ? style.ruby2_letter_spacing : style.ruby_letter_spacing;
                const auto kind = lower ? PreviewRenderer::LayoutKind::lower_token
                    : PreviewRenderer::LayoutKind::masked;
                for (const auto& sample : samples) for (const int timing_count : {0, 1, 2}) {
                    RubyTrack track; track.text = sample[0] + sample[1];
                    std::vector<std::string> chunks{track.text};
                    if (timing_count == 1) track.timing.push_back({track.text, 0});
                    if (timing_count == 2) {
                        track.timing = {{sample[0], 0}, {sample[1], 5}};
                        chunks = sample;
                    }
                    const auto total = fixture.text_width(track.text, size, bold, spacing);
                    expect_near(fixture.renderer.measure_track(track, style, lower), total,
                        "ruby track measurement uses its own drawing weight");
                    constexpr float group_x = 140, group_width = 270;
                    fixture.begin();
                    fixture.renderer.draw_ruby_track(track, group_x, 120, group_width,
                        0, 10, 2.5, style, 1, {}, lower);
                    fixture.end();
                    const auto draws = draws_of(fixture, kind, size);
                    const auto expected_count = lower ? chunks.size()
                        : PreviewRenderer::utf8_chars(track.text).size();
                    check(draws.size() == expected_count, "ruby character/token draw count");
                    std::size_t index{};
                    float cursor = group_x + (group_width - total) / 2;
                    for (std::size_t chunk = 0; chunk < chunks.size(); ++chunk) {
                        const double progress = timing_count == 2
                            ? (chunk == 0 ? 0.5 : 0.0) : 0.25;
                        const auto chars = PreviewRenderer::utf8_chars(chunks[chunk]);
                        if (lower) {
                            const auto width = fixture.text_width(chunks[chunk], size, bold, spacing);
                            if (index < draws.size()) {
                                expect_near(draws[index].x, cursor, "ruby2 token centering / cursor");
                                expect_near(draws[index].width, width, "ruby2 token advance");
                                expect_near(static_cast<float>(draws[index].progress),
                                    static_cast<float>(progress), "ruby2 retains token timing");
                                check(draws[index].bold == bold, "ruby2 draw Bold state");
                            }
                            ++index; cursor += width;
                        } else {
                            for (std::size_t i = 0; i < chars.size(); ++i) {
                                const auto width = fixture.advance(chars[i], size, bold);
                                if (index < draws.size()) {
                                    expect_near(draws[index].x, cursor, "upper ruby centering / cursor");
                                    expect_near(draws[index].width, width, "upper ruby character advance");
                                    expect_near(static_cast<float>(draws[index].progress),
                                        static_cast<float>(std::clamp(progress * chars.size() - i, 0.0, 1.0)),
                                        "upper ruby retains per-character timing");
                                    check(draws[index].bold == bold, "upper ruby draw Bold state");
                                }
                                ++index; cursor += width;
                                if (i + 1 < chars.size()) cursor += spacing;
                            }
                        }
                        if (chunk + 1 < chunks.size()) cursor += spacing;
                    }
                    expect_near(cursor, group_x + (group_width + total) / 2,
                        "ruby advance ends symmetrically around the group center");
                }
            }
        }
    }
}

void ruby_group_tests(LayoutFixture& fixture) {
    for (const auto& font : fonts) {
        if (!fixture.installed(font.installed_family)) continue;
        fixture.use({font.request, L"sans-serif"});
        for (int flags = 0; flags < 8; ++flags) for (const bool isolate : {false, true}) {
            RenderStyle style; style.stroke_width = 0; style.role_colors = false;
            style.font_bold = (flags & 1) != 0;
            style.ruby_bold = (flags & 2) != 0;
            style.ruby2_bold = (flags & 4) != 0;
            style.ruby_isolate = isolate; style.letter_spacing = 6.75F;
            style.ruby_letter_spacing = 2.75F; style.ruby2_letter_spacing = 3.25F;
            auto line = plain_line("MWm"); line.chars[0].ruby_span = 2;
            line.chars[0].ruby_above.text = "Romaji AWm";
            line.chars[0].ruby_below.text = "MWm109";
            line.chars[2].ruby_above.text = "AWm";
            line.chars[2].ruby_below.text = "iWm09";
            float base[2]{}, upper[2]{}, lower[2]{}, ruby[2]{}, width[2]{};
            for (int group = 0; group < 2; ++group) {
                const auto first = group == 0 ? 0U : 2U;
                const auto count = group == 0 ? 2U : 1U;
                const auto text = group == 0 ? "MW" : "m";
                base[group] = fixture.text_width(text, style.font_size,
                    style.font_bold, style.letter_spacing);
                upper[group] = fixture.text_width(line.chars[first].ruby_above.text,
                    style.ruby_size, style.ruby_bold, style.ruby_letter_spacing);
                lower[group] = fixture.text_width(line.chars[first].ruby_below.text,
                    style.ruby2_size, style.ruby2_bold, style.ruby2_letter_spacing);
                ruby[group] = std::max(upper[group], lower[group]);
                width[group] = isolate ? std::max(base[group], ruby[group]) : base[group];
                expect_near(fixture.renderer.group_ruby_width(line, first, style), ruby[group],
                    "ruby group width accounts for both independent Bold states");
                expect_near(fixture.renderer.group_width(line, first, count, style), width[group],
                    "ruby isolate group width follows resolved metrics");
            }
            const auto extra = isolate ? 0.0F : std::max(0.0F,
                (ruby[0] - base[0] + ruby[1] - base[1]) / 2
                    + style.ruby_letter_spacing - style.letter_spacing);
            expect_near(fixture.renderer.ruby_extra_gap(line, 0, 2, 2, 1, style), extra,
                "adjacent ruby avoidance uses matching advances");
            const auto total = width[0] + width[1] + style.letter_spacing + extra;
            expect_near(fixture.renderer.line_width(line, style), total,
                "ruby isolate / avoidance contributes correct line width");
            fixture.line(line, style, 1);
            const auto main = draws_of(fixture, PreviewRenderer::LayoutKind::masked, style.font_size);
            const auto above = draws_of(fixture, PreviewRenderer::LayoutKind::masked, style.ruby_size);
            const auto below = draws_of(fixture, PreviewRenderer::LayoutKind::lower_token, style.ruby2_size);
            check(main.size() == 3 && below.size() == 2, "grouped main and ruby2 draw counts");
            float group_x = 1280 - style.line2_right - total;
            std::size_t mi{}, ui{};
            for (int group = 0; group < 2; ++group) {
                const auto main_text = group == 0 ? "MW" : "m";
                float cursor = group_x + (width[group] - base[group]) / 2;
                for (const auto& ch : PreviewRenderer::utf8_chars(main_text)) {
                    if (mi < main.size()) expect_near(main[mi].x, cursor, "main centered inside ruby group");
                    ++mi; cursor += fixture.advance(ch, style.font_size, style.font_bold) + style.letter_spacing;
                }
                cursor = group_x + (width[group] - upper[group]) / 2;
                for (const auto& ch : PreviewRenderer::utf8_chars(line.chars[group == 0 ? 0 : 2].ruby_above.text)) {
                    if (ui < above.size()) expect_near(above[ui].x, cursor, "upper ruby centered inside group");
                    ++ui; cursor += fixture.advance(ch, style.ruby_size, style.ruby_bold) + style.ruby_letter_spacing;
                }
                if (group < static_cast<int>(below.size())) {
                    expect_near(below[group].x, group_x + (width[group] - lower[group]) / 2,
                        "ruby2 centered inside group");
                    expect_near(below[group].width, lower[group], "ruby2 group drawing advance");
                }
                group_x += width[group];
                if (group == 0) group_x += style.letter_spacing + extra;
            }
            check(ui == above.size(), "upper ruby group draw count");
            expect_near(group_x, 1280 - style.line2_right, "ruby group right edge remains aligned");
        }
    }
}

} // namespace

int main() {
    require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM initialization");
    {
        LayoutFixture fixture;
        main_layout_tests(fixture);
        ruby_track_tests(fixture);
        ruby_group_tests(fixture);
    }
    CoUninitialize();
    std::cout << "Lyric layout metrics: " << checks << " checks, " << failures << " failures.\n";
    return failures == 0 ? 0 : 1;
}
