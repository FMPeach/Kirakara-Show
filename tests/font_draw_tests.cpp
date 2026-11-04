#include "../src/win32/font_resolver.hpp"

#include <windows.h>
#include <d2d1.h>
#include <dwrite_3.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace kirakara::show::win32::detail;

namespace {
int checks{};
void expect(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
void succeeded(HRESULT hr, const char* message) { expect(SUCCEEDED(hr), message); }
void close_to(double a, double b, const char* message) {
    expect(std::abs(a - b) < 0.0001, message);
}
template<class T> void release(T*& value) {
    if (value) { value->Release(); value = nullptr; }
}
bool installed(IDWriteFontCollection* fonts, const wchar_t* name) {
    UINT32 index{}; BOOL exists{};
    return SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists;
}
std::vector<DWRITE_FONT_AXIS_VALUE> axes_of(IDWriteFontFace* face) {
    IDWriteFontFace5* face5{};
    if (FAILED(face->QueryInterface(IID_PPV_ARGS(&face5)))) return {};
    const auto count = face5->GetFontAxisValueCount();
    std::vector<DWRITE_FONT_AXIS_VALUE> values(count);
    succeeded(face5->GetFontAxisValues(values.data(), count), "read font axes");
    release(face5); return values;
}
int weight_of(IDWriteFontFace* face) {
    IDWriteFontFace3* face3{};
    succeeded(face->QueryInterface(IID_PPV_ARGS(&face3)), "actual face weight");
    const auto weight = face3->GetWeight(); release(face3); return weight;
}

struct Run {
    float baseline_x{}, baseline_y{};
    int base_weight{};
    DWRITE_FONT_SIMULATIONS base_simulations{}, outline_simulations{};
    std::vector<UINT16> indices;
    std::vector<float> advances;
    std::vector<DWRITE_GLYPH_OFFSET> offsets;
    IDWriteFontFace* cached_outline{}; // Retained by the resolver for simulated faces.
};
struct Result { float width{}, area{}; std::vector<Run> runs; };

class Collector final : public IDWriteTextRenderer {
public:
    Collector(FontResolver& fonts, const ResolvedFont& font, ID2D1Factory* d2d)
        : fonts_(fonts), font_(font), d2d_(d2d) {}
    Result result;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteTextRenderer)
                || iid == __uuidof(IDWritePixelSnapping)) {
            *value = static_cast<IDWriteTextRenderer*>(this); AddRef(); return S_OK;
        }
        *value = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* value) override { *value = TRUE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* value) override { *value = {1,0,0,1,0,0}; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* value) override { *value = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE,
        const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        Run saved;
        saved.baseline_x = x; saved.baseline_y = y;
        saved.base_weight = weight_of(run->fontFace);
        saved.base_simulations = run->fontFace->GetSimulations();
        saved.indices.assign(run->glyphIndices, run->glyphIndices + run->glyphCount);
        saved.advances.assign(run->glyphAdvances, run->glyphAdvances + run->glyphCount);
        if (run->glyphOffsets) saved.offsets.assign(run->glyphOffsets, run->glyphOffsets + run->glyphCount);

        IDWriteFontFace* outline{}; IDWriteFontFace* again{};
        succeeded(fonts_.outline_face(font_, run->fontFace, &outline), "select outline face");
        succeeded(fonts_.outline_face(font_, run->fontFace, &again), "reuse outline face");
        expect(outline == again, "outline face is reused, including its native simulation");
        saved.outline_simulations = outline->GetSimulations();
        saved.cached_outline = outline;
        const bool needs_bold = font_.explicit_face && font_.additional_bold
            && saved.base_weight < 600 && !(saved.base_simulations & DWRITE_FONT_SIMULATIONS_BOLD);
        if (needs_bold) {
            expect(outline != run->fontFace, "draw-only Bold has a separate face");
            expect(saved.outline_simulations & DWRITE_FONT_SIMULATIONS_BOLD, "native BOLD flag");
            const auto before = axes_of(run->fontFace), after = axes_of(outline);
            expect(before.size() == after.size(), "native simulation preserves axis count");
            for (std::size_t i = 0; i < before.size(); ++i) {
                expect(before[i].axisTag == after[i].axisTag && before[i].value == after[i].value,
                    "native simulation preserves every axis value");
            }
        } else {
            expect(outline == run->fontFace, "real Bold or unrequested simulation keeps the original face");
        }
        release(again); release(outline);

        ID2D1PathGeometry* geometry{}; ID2D1GeometrySink* sink{};
        succeeded(d2d_->CreatePathGeometry(&geometry), "outline geometry");
        succeeded(geometry->Open(&sink), "outline sink");
        succeeded(fonts_.glyph_run_outline(font_, *run, sink), "outline with base layout positions");
        succeeded(sink->Close(), "close outline sink");
        float area{};
        succeeded(geometry->ComputeArea(nullptr, 0.001F, &area), "outline area");
        result.area += area;
        for (UINT32 i = 0; i < run->glyphCount; ++i) {
            expect(saved.indices[i] == run->glyphIndices[i], "outline preserves glyph indices");
            expect(saved.advances[i] == run->glyphAdvances[i], "outline preserves base advances");
            if (run->glyphOffsets) expect(saved.offsets[i].advanceOffset == run->glyphOffsets[i].advanceOffset
                    && saved.offsets[i].ascenderOffset == run->glyphOffsets[i].ascenderOffset,
                "outline preserves base offsets");
        }
        result.runs.push_back(std::move(saved));
        release(sink); release(geometry); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }
private:
    ULONG refs_{1};
    FontResolver& fonts_;
    const ResolvedFont& font_;
    ID2D1Factory* d2d_{};
};

Result render(IDWriteFactory* dwrite, ID2D1Factory* d2d, FontResolver& fonts,
    const ResolvedFont& font, const std::wstring& text) {
    auto* format = fonts.create_format(font, 22);
    expect(format && static_cast<int>(format->GetFontWeight()) == font.layout_weight,
        "format carries only the layout weight");
    IDWriteTextLayout* layout{};
    succeeded(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
        format, 4096, 200, &layout), "font drawing layout");
    IDWriteTypography* typography{}; succeeded(dwrite->CreateTypography(&typography), "typography");
    for (const auto tag : {DWRITE_FONT_FEATURE_TAG_KERNING, DWRITE_FONT_FEATURE_TAG_STANDARD_LIGATURES,
        DWRITE_FONT_FEATURE_TAG_CONTEXTUAL_LIGATURES}) succeeded(typography->AddFontFeature({tag, 0}), "disable text feature");
    succeeded(layout->SetTypography(typography, {0, static_cast<UINT32>(text.size())}), "layout typography");
    Collector collector(fonts, font, d2d);
    DWRITE_TEXT_METRICS metrics{}; succeeded(layout->GetMetrics(&metrics), "base layout metrics");
    collector.result.width = metrics.widthIncludingTrailingWhitespace;
    succeeded(layout->Draw(nullptr, &collector, 11.25F, 7.5F), "collect drawing face and geometry");
    release(typography); release(layout); release(format);
    return std::move(collector.result);
}

void same_layout(const Result& normal, const Result& bold) {
    close_to(normal.width, bold.width, "draw-only Bold keeps layout advance");
    expect(normal.runs.size() == bold.runs.size(), "draw-only Bold keeps shaping runs");
    for (std::size_t i = 0; i < normal.runs.size(); ++i) {
        const auto& a = normal.runs[i]; const auto& b = bold.runs[i];
        expect(a.indices == b.indices && a.advances == b.advances, "base glyph identity and advances match");
        expect(a.baseline_x == b.baseline_x && a.baseline_y == b.baseline_y, "base baseline positions match");
        expect(a.offsets.size() == b.offsets.size(), "base offset counts match");
        for (std::size_t j = 0; j < a.offsets.size(); ++j) expect(
            a.offsets[j].advanceOffset == b.offsets[j].advanceOffset
                && a.offsets[j].ascenderOffset == b.offsets[j].ascenderOffset,
            "base glyph offsets match");
    }
}

IDWriteFont* match(IDWriteFontCollection* fonts, const wchar_t* family_name, int weight) {
    UINT32 index{}; BOOL exists{};
    succeeded(fonts->FindFamilyName(family_name, &index, &exists), "find candidate family");
    expect(exists, "candidate family exists");
    IDWriteFontFamily* family{}; succeeded(fonts->GetFontFamily(index, &family), "candidate family");
    IDWriteFont* font{};
    succeeded(family->GetFirstMatchingFont(static_cast<DWRITE_FONT_WEIGHT>(weight),
        DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font), "candidate font");
    release(family); return font;
}

void fallback_test(IDWriteFactory* dwrite, ID2D1Factory* d2d,
    IDWriteFontCollection* system, const wchar_t* fallback_family) {
    auto* latin_font = match(system, L"Arial", 400);
    auto* fallback_font = match(system, fallback_family, 700);
    expect(fallback_font->GetWeight() >= 600
            && fallback_font->GetSimulations() == DWRITE_FONT_SIMULATIONS_NONE,
        "fallback fixture has a real Bold face");
    IDWriteFontFace* latin_face{}; IDWriteFontFace* fallback_face{};
    succeeded(latin_font->CreateFontFace(&latin_face), "fallback Latin coverage");
    succeeded(fallback_font->CreateFontFace(&fallback_face), "fallback candidate coverage");
    wchar_t missing{};
    for (UINT32 ch = 0x100; ch < 0xA000; ++ch) {
        UINT16 latin_glyph{}, fallback_glyph{};
        succeeded(latin_face->GetGlyphIndices(&ch, 1, &latin_glyph), "Latin coverage lookup");
        succeeded(fallback_face->GetGlyphIndices(&ch, 1, &fallback_glyph), "fallback coverage lookup");
        if (!latin_glyph && fallback_glyph) { missing = static_cast<wchar_t>(ch); break; }
    }
    release(fallback_face); release(latin_face);
    expect(missing != 0, "a real glyph missing in Arial is available in the fallback");
    std::wcout << L"Fallback fixture: " << fallback_family << L" glyph=U+"
        << std::hex << static_cast<unsigned>(missing) << std::dec
        << L" weight=" << fallback_font->GetWeight()
        << L" simulations=" << fallback_font->GetSimulations() << L'\n';
    const std::wstring mixed_text{L'A', missing};
    IDWriteFactory3* factory3{};
    succeeded(dwrite->QueryInterface(IID_PPV_ARGS(&factory3)), "fallback test factory3");
    IDWriteFontSetBuilder* builder{}; succeeded(factory3->CreateFontSetBuilder(&builder), "fallback set builder");
    auto add = [&](IDWriteFont* font, const wchar_t* alias) {
        IDWriteFont3* font3{}; succeeded(font->QueryInterface(IID_PPV_ARGS(&font3)), "fallback font3");
        IDWriteFontFaceReference* reference{}; succeeded(font3->GetFontFaceReference(&reference), "fallback reference");
        const DWRITE_FONT_PROPERTY properties[]{
            {DWRITE_FONT_PROPERTY_ID_FAMILY_NAME, alias, L"en-us"},
            {DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FAMILY_NAME, alias, L"en-us"}};
#ifdef __MINGW32__
        succeeded(builder->AddFontFaceReference_(reference, properties, 2), "add fallback face");
#else
        succeeded(builder->AddFontFaceReference(reference, properties, 2), "add fallback face");
#endif
        release(reference); release(font3);
    };
    add(latin_font, L"Kirakara Latin");
    add(fallback_font, L"Kirakara Fallback");
    release(fallback_font); release(latin_font);
    IDWriteFontSet* set{}; succeeded(builder->CreateFontSet(&set), "fallback private set");
    IDWriteFontCollection1* collection{};
    succeeded(factory3->CreateFontCollectionFromFontSet(set, &collection), "fallback private collection");
    {
        FontResolver fonts; expect(fonts.initialize(dwrite, collection), "fallback resolver init");
        const std::vector<std::wstring> families{L"Kirakara Latin Regular", L"Kirakara Fallback"};
        const auto& plain = fonts.resolve(families, false);
        const auto& bold = fonts.resolve(families, true);
        const auto normal_result = render(dwrite, d2d, fonts, plain, mixed_text);
        const auto bold_result = render(dwrite, d2d, fonts, bold, mixed_text);
        same_layout(normal_result, bold_result);
        bool saw_light{}, saw_bold{};
        for (const auto& run : bold_result.runs) {
            if (run.base_weight < 600) {
                saw_light = true; expect(run.outline_simulations & DWRITE_FONT_SIMULATIONS_BOLD,
                    "light primary run receives draw-only simulation");
            } else {
                saw_bold = true; expect(run.outline_simulations == run.base_simulations,
                    "real Bold fallback is kept without a second simulation");
            }
        }
        expect(saw_light && saw_bold, "mixed primary/fallback faces both exercised");
    }
    release(collection); release(set); release(builder); release(factory3);
}
} // namespace

int main() {
    succeeded(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "COM");
    IDWriteFactory* dwrite{};
    succeeded(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&dwrite)), "DirectWrite");
    ID2D1Factory* d2d{};
    succeeded(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d), "Direct2D");
    IDWriteFontCollection* system{}; succeeded(dwrite->GetSystemFontCollection(&system), "system fonts");
    {
        FontResolver fonts; expect(fonts.initialize(dwrite), "draw font resolver");
        const std::vector<std::wstring> texts{L"m", L"i", L"1", L".", L"mawa", L"ryoku", L"MWm09"};
        for (const auto* family : {L"Arial Regular", L"Arial Bold", L"Noto Sans JP Regular",
            L"Noto Sans JP Medium", L"Noto Sans JP SemiBold", L"Noto Sans JP Heavy",
            L"Noto Sans JP Bold", L"MotoyaLMaru W3 Mono Regular"}) {
            const bool noto = std::wstring_view{family}.starts_with(L"Noto");
            if (noto && !installed(system, L"Noto Sans JP")) continue;
            if (std::wstring_view{family}.starts_with(L"Motoya")
                    && !installed(system, L"MotoyaLMaru W3 Mono")) continue;
            const auto& plain = fonts.resolve({family}, false);
            const auto& bold = fonts.resolve({family}, true);
            expect(plain.layout_weight == bold.layout_weight, "explicit face layout state is stable");
            expect(plain.cache_key != bold.cache_key, "normal/Bold geometry caches have independent identities");
            IDWriteFontFace* cached_outline{};
            for (const auto& text : texts) {
                const auto normal = render(dwrite, d2d, fonts, plain, text);
                const auto heavy = render(dwrite, d2d, fonts, bold, text);
                same_layout(normal, heavy);
                if (bold.draw_bold_simulation) {
                    expect(heavy.area > normal.area * 1.01F, "native Bold increases the actual filled glyph outline");
                    expect(heavy.runs.size() == 1, "Latin sample uses one base face");
                    if (cached_outline) expect(cached_outline == heavy.runs[0].cached_outline,
                        "different texts and new layouts reuse the same simulated face");
                    cached_outline = heavy.runs[0].cached_outline;
                } else close_to(normal.area, heavy.area, "intrinsically bold face geometry is unchanged");
                std::wcout << family << L" text=" << text << L" advance=" << heavy.width
                    << L" area=" << normal.area << L" -> " << heavy.area << L'\n';
                const auto restored = render(dwrite, d2d, fonts, plain, text);
                close_to(normal.area, restored.area, "normal outline remains unchanged after Bold cache use");
            }
        }
        if (installed(system, L"Noto Sans JP")) {
            const auto& medium = fonts.resolve({L"Noto Sans JP Medium"}, true);
            const auto& family = fonts.resolve({L"Noto Sans JP"}, true);
            expect(family.layout_weight == 700 && !family.draw_bold_simulation,
                "plain family Bold selects a real Bold layout");
            const auto real_bold = render(dwrite, d2d, fonts, family, L"ryoku");
            for (const auto& run : real_bold.runs) expect(run.base_weight >= 600
                && run.outline_simulations == DWRITE_FONT_SIMULATIONS_NONE, "plain Noto Bold uses a real face");
            auto* base_font = match(system, L"Noto Sans JP", 500);
            IDWriteFontFace* base{}; succeeded(base_font->CreateFontFace(&base), "base Medium face");
            IDWriteFontFace5* base5{};
            if (SUCCEEDED(base->QueryInterface(IID_PPV_ARGS(&base5))) && base5->HasVariations()) {
                const auto original_axes = axes_of(base);
                bool has_500{};
                for (const auto& axis : original_axes) if (axis.axisTag == DWRITE_FONT_AXIS_TAG_WEIGHT) {
                    expect(axis.value == 500, "Medium base is wght=500"); has_500 = true;
                }
                expect(has_500, "variable Medium has a weight axis");
                IDWriteFontFace* outline{};
                succeeded(fonts.outline_face(medium, base, &outline), "variable Medium outline face");
                const auto outline_axes = axes_of(outline);
                expect(original_axes.size() == outline_axes.size(), "variable outline axis count");
                for (std::size_t i = 0; i < original_axes.size(); ++i) expect(
                    original_axes[i].axisTag == outline_axes[i].axisTag
                        && original_axes[i].value == outline_axes[i].value, "variable Medium keeps its full axis identity");
                IDWriteFontResource* resource{}; succeeded(base5->GetFontResource(&resource), "variable resource");
                auto axes = original_axes;
                for (auto& axis : axes) if (axis.axisTag == DWRITE_FONT_AXIS_TAG_WEIGHT) axis.value = 400;
                IDWriteFontFace5* regular{};
                succeeded(resource->CreateFontFace(DWRITE_FONT_SIMULATIONS_NONE, axes.data(),
                    static_cast<UINT32>(axes.size()), &regular), "second variable instance");
                IDWriteFontFace* regular_outline{};
                succeeded(fonts.outline_face(medium, regular, &regular_outline), "second instance outline");
                expect(outline != regular_outline, "distinct variable instances have distinct cached simulation faces");
                for (const auto& axis : axes_of(regular_outline)) if (axis.axisTag == DWRITE_FONT_AXIS_TAG_WEIGHT)
                    expect(axis.value == 400, "second cached outline retains its own weight axis");
                release(regular_outline); release(regular); release(resource); release(outline);
            }
            release(base5); release(base); release(base_font);
        } else std::cout << "SKIP: optional Noto face/axis cases (font not installed)\n";
        fallback_test(dwrite, d2d, system, L"Segoe UI");
        if (installed(system, L"Noto Sans JP"))
            fallback_test(dwrite, d2d, system, L"Noto Sans JP");
    }
    release(system); release(d2d); release(dwrite); CoUninitialize();
    std::cout << "Draw-only font Bold tests: " << checks << " checks passed.\n";
}
