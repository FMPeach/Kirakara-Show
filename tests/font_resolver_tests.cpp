#include "../src/win32/font_resolver.hpp"

#include <windows.h>
#include <dwrite_3.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace kirakara::show::win32::detail;

namespace {
void expect(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
template <typename T> void release(T*& value) {
    if (value) { value->Release(); value = nullptr; }
}
bool installed(IDWriteFontCollection* collection, const wchar_t* name) {
    UINT32 index{}; BOOL exists{};
    return SUCCEEDED(collection->FindFamilyName(name, &index, &exists)) && exists;
}

class RunCollector final : public IDWriteTextRenderer {
public:
    explicit RunCollector(IDWriteFontCollection* collection) : collection_(collection) {}
    struct Run { std::wstring family; int weight; DWRITE_FONT_SIMULATIONS simulations; };
    std::vector<Run> runs;
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
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* value) override {
        *value = TRUE; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* value) override {
        *value = {1, 0, 0, 1, 0, 0}; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* value) override {
        *value = 1; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT, FLOAT, DWRITE_MEASURING_MODE,
        const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        IDWriteFont* font{}; IDWriteFontFamily* family{}; IDWriteLocalizedStrings* names{};
        expect(SUCCEEDED(collection_->GetFontFromFontFace(run->fontFace, &font)),
            "resolve actual glyph face");
        font->GetFontFamily(&family); family->GetFamilyNames(&names);
        UINT32 index{}, length{}; BOOL exists{};
        names->FindLocaleName(L"en-us", &index, &exists); if (!exists) index = 0;
        names->GetStringLength(index, &length);
        std::wstring name(length + 1, L'\0'); names->GetString(index, name.data(), length + 1);
        name.resize(length);
        runs.push_back({name, static_cast<int>(font->GetWeight()), run->fontFace->GetSimulations()});
        release(names); release(family); release(font);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }
private:
    ULONG refs_{1};
    IDWriteFontCollection* collection_{};
};

void test_exact_family(IDWriteFactory* factory, IDWriteFontCollection* system) {
    // A private in-memory collection gives a real family a misleading suffix,
    // without installing fonts or requiring any Noto font on CI.
    IDWriteFactory3* factory3{};
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory3)))) {
        std::cout << "SKIP: DirectWrite 3 private font-set test\n"; return;
    }
    IDWriteFontFamily* family{}; system->GetFontFamily(0, &family);
    IDWriteFont* font{}; family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font);
    IDWriteFont3* font3{}; expect(SUCCEEDED(font->QueryInterface(IID_PPV_ARGS(&font3))), "font3");
    IDWriteFontFaceReference* reference{}; font3->GetFontFaceReference(&reference);
    IDWriteFontSetBuilder* builder{}; factory3->CreateFontSetBuilder(&builder);
    const DWRITE_FONT_PROPERTY properties[]{
        {DWRITE_FONT_PROPERTY_ID_FAMILY_NAME, L"Kirakara Test Light", L"en-us"},
        {DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FAMILY_NAME, L"Kirakara Test Light", L"en-us"},
    };
#ifdef __MINGW32__
    const auto added = builder->AddFontFaceReference_(reference, properties, 2);
#else
    const auto added = builder->AddFontFaceReference(reference, properties, 2);
#endif
    expect(SUCCEEDED(added), "private aliased font reference");
    IDWriteFontSet* set{}; builder->CreateFontSet(&set);
    IDWriteFontCollection1* collection{}; factory3->CreateFontCollectionFromFontSet(set, &collection);
    expect(installed(collection, L"Kirakara Test Light"), "private family alias exists");
    {
        FontResolver resolver; expect(resolver.initialize(factory, collection), "private resolver");
        const auto& result = resolver.resolve({L"Kirakara Test Light"}, false);
        expect(result.original_family_exists, "exact family beats suffix parsing");
        expect(!result.request.has_explicit_weight_suffix && result.request.base_weight == 400,
            "real family ending Light is not stripped");
        expect(result.family_name == L"Kirakara Test Light", "original family retained");
    }
    release(collection); release(set); release(builder); release(reference);
    release(font3); release(font); release(family); release(factory3);
}
}

int main() {
    expect(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM initialization");
    IDWriteFactory* factory{};
    expect(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&factory))), "DirectWrite factory");
    IDWriteFontCollection* collection{}; factory->GetSystemFontCollection(&collection);
    {
        FontResolver resolver; expect(resolver.initialize(factory), "font resolver initialization");
        const auto& legacy = resolver.resolve({L"Microsoft YaHei"}, true);
        expect(legacy.layout_weight == 700 && !legacy.draw_bold_simulation,
            "legacy family Bold is still a real 700 request");
        expect(&legacy == &resolver.resolve({L"Microsoft YaHei"}, true), "resolution is cached");
        auto* format = resolver.create_format(legacy, 62);
        expect(format && format->GetFontWeight() == DWRITE_FONT_WEIGHT_BOLD, "numeric text format");
        release(format);
        const auto& spaced = resolver.resolve({L"  Arial  "}, false);
        expect(spaced.original_family_exists && spaced.request.requested_name == L"  Arial  ",
            "trimmed lookup retains the original requested name");
        if (installed(collection, L"Segoe UI")) {
            const auto& semibold = resolver.resolve({L"Segoe UI SemiBold"}, true);
            expect(semibold.layout_weight == 600 && semibold.matched_weight >= 600
                    && !semibold.draw_bold_simulation,
                "intrinsic SemiBold is not made heavier than Edge's face");
        }

        const auto& missing = resolver.resolve({L"Kirakara Missing Heavy", L"Arial"}, true);
        expect(missing.request.requested_name == L"Kirakara Missing Heavy", "missing original retained");
        expect(missing.request.family_name == L"Kirakara Missing Heavy", "unusable stripped family rejected");
        expect(missing.layout_weight == 900, "missing Heavy carries weight into fallback");
        expect(missing.family_name == L"Arial", "next installed family selected");
        expect(missing.matched_base_weight > 0, "fallback records its actual matched base weight");
        test_exact_family(factory, collection);

        if (installed(collection, L"Noto Sans JP")) {
            struct Case { const wchar_t* suffix; int base; };
            const Case cases[]{{L"Light", 300}, {L"Regular", 400}, {L"Medium", 500},
                {L"SemiBold", 600}, {L"ExtraBold", 800}, {L"Heavy", 900}};
            for (const auto& test : cases) {
                const auto name = std::wstring{L"Noto Sans JP "} + test.suffix;
                for (const bool bold : {false, true}) {
                    const auto& result = resolver.resolve({name}, bold);
                    expect(result.family_name == L"Noto Sans JP", "named Noto family");
                    expect(result.request.base_weight == test.base, "named Noto base weight");
                    expect(result.layout_weight == test.base, "named face layout retains base weight");
                    expect((result.layout_simulations & DWRITE_FONT_SIMULATIONS_BOLD) == 0,
                        "named Noto layout uses a real base face");
                    expect(result.draw_bold_simulation == (bold && result.matched_weight < 600),
                        "draw-only simulation follows the actual base face");
                    std::wcout << name << L" bold=" << bold << L" base=" << result.request.base_weight
                        << L" layout=" << result.layout_weight << L" matched=" << result.matched_weight << L'\n';
                }
            }
            const auto& medium = resolver.resolve({L"Noto Sans JP Medium"}, true);
            expect(medium.layout_weight == 500 && medium.matched_weight == 500
                    && medium.additional_bold && medium.draw_bold_simulation,
                "Medium + Bold keeps Medium layout and requests native outline simulation");
            expect(medium.cache_key != resolver.resolve({L"Noto Sans JP Medium"}, false).cache_key,
                "equal layout weights retain separate normal/Bold geometry identities");
            const auto& heavy = resolver.resolve({L"Noto Sans JP Heavy"}, true);
            expect(heavy.layout_weight == 900 && !heavy.draw_bold_simulation, "Heavy stays real Heavy");
            expect(medium.cache_key != heavy.cache_key, "final font identity separates caches");
            const auto& full_bold = resolver.resolve({L"Noto Sans JP Bold"}, true);
            expect(full_bold.full_face_match && full_bold.layout_weight == 700,
                "confirmed full Bold face not doubled");
            const auto& full_plain = resolver.resolve({L"Noto Sans JP Bold"}, false);
            expect(full_plain.layout_weight == 700, "named Bold face remains Bold when UI toggle off");

            const auto& fallback = resolver.resolve({L"Arial Heavy", L"Noto Sans JP"}, false);
            expect(fallback.layout_weight == 900, "fallback layout weight retained");
            auto* fallback_format = resolver.create_format(fallback, 62);
            IDWriteTextLayout* layout{};
            expect(SUCCEEDED(factory->CreateTextLayout(L"A永", 2, fallback_format, 1000, 100, &layout)),
                "fallback layout");
            RunCollector collector(collection);
            expect(SUCCEEDED(layout->Draw(nullptr, &collector, 0, 0)), "fallback glyph collection");
            bool saw_japanese{};
            for (const auto& run : collector.runs) if (run.family == L"Noto Sans JP") {
                saw_japanese = true; expect(run.weight >= 800, "missing glyph fallback stays Heavy");
            }
            expect(saw_japanese, "configured fallback family used for Japanese glyph");
            release(layout); release(fallback_format);
        } else std::cout << "SKIP: Noto Sans JP integration cases (font not installed)\n";

        if (installed(collection, L"MotoyaLMaru W3 Mono")) {
            const auto& regular_only = resolver.resolve({L"MotoyaLMaru W3 Mono"}, true);
            expect(regular_only.layout_weight == 700 && regular_only.matched_weight >= 600,
                "Regular-only font permits DirectWrite native Bold matching");
        }
    }
    release(collection); release(factory); CoUninitialize();
    std::cout << "DirectWrite font resolution tests passed.\n";
}
