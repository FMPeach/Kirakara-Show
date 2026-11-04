#include "font_resolver.hpp"

#include <dwrite_3.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <string_view>

namespace kirakara::show::win32::detail {
namespace {

template <typename T> void release(T*& value) {
    if (value) { value->Release(); value = nullptr; }
}

std::wstring localized_name(IDWriteLocalizedStrings* names) {
    if (!names || names->GetCount() == 0) return {};
    UINT32 index{}, length{};
    BOOL exists{};
    names->FindLocaleName(L"en-us", &index, &exists);
    if (!exists) index = 0;
    if (FAILED(names->GetStringLength(index, &length))) return {};
    std::wstring result(length + 1, L'\0');
    if (FAILED(names->GetString(index, result.data(), length + 1))) return {};
    result.resize(length);
    return result;
}

std::wstring family_name(IDWriteFontFamily* family) {
    IDWriteLocalizedStrings* names{};
    family->GetFamilyNames(&names);
    const auto result = localized_name(names);
    release(names);
    return result;
}

IDWriteFontFamily* find_family(IDWriteFontCollection* collection,
    const std::wstring& name) {
    if (!collection || name.empty()) return nullptr;
    UINT32 index{};
    BOOL exists{};
    IDWriteFontFamily* result{};
    if (SUCCEEDED(collection->FindFamilyName(name.c_str(), &index, &exists))
            && exists) collection->GetFontFamily(index, &result);
    return result;
}

IDWriteFont* match(IDWriteFontFamily* family, int weight,
    DWRITE_FONT_STYLE style, DWRITE_FONT_STRETCH stretch) {
    IDWriteFont* font{};
    if (family) family->GetFirstMatchingFont(
        static_cast<DWRITE_FONT_WEIGHT>(weight), stretch, style, &font);
    return font;
}

// Full-name matching is performed by DirectWrite's font set, not guessed by
// stripping "Bold" or by choosing an .otf/.ttf file ourselves.
IDWriteFont* find_full_face(IDWriteFontCollection* collection,
    const std::wstring& name) {
    if (!collection) return nullptr;
    IDWriteFontCollection1* collection1{};
    IDWriteFontSet* set{};
    IDWriteFontSet* matches{};
    IDWriteFontFaceReference* reference{};
    IDWriteFontFace3* face{};
    IDWriteFont* result{};
    const DWRITE_FONT_PROPERTY property{
        DWRITE_FONT_PROPERTY_ID_FULL_NAME, name.c_str(), nullptr};
    if (SUCCEEDED(collection->QueryInterface(IID_PPV_ARGS(&collection1)))
            && SUCCEEDED(collection1->GetFontSet(&set))
            && SUCCEEDED(set->GetMatchingFonts(&property, 1, &matches))
            && matches->GetFontCount() > 0
            && SUCCEEDED(matches->GetFontFaceReference(0, &reference))
            && SUCCEEDED(reference->CreateFontFace(&face))) {
        collection->GetFontFromFontFace(face, &result);
    }
    release(face); release(reference); release(matches);
    release(set); release(collection1);
    return result;
}

std::wstring lower_ascii(std::wstring value) {
    for (auto& ch : value) if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
    return value;
}

std::wstring trimmed(std::wstring_view value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring_view::npos) return {};
    return std::wstring{value.substr(first,
        value.find_last_not_of(L" \t\r\n") - first + 1)};
}

void append_unique(std::vector<std::wstring>& values, std::wstring value) {
    if (value.empty()) return;
    if (std::none_of(values.begin(), values.end(), [&](const auto& existing) {
            return lower_ascii(existing) == lower_ascii(value);
        })) values.push_back(std::move(value));
}

std::vector<std::wstring> expanded_families(
    const std::vector<std::wstring>& requested) {
    std::vector<std::wstring> result;
    for (const auto& family : requested) {
        const auto generic = lower_ascii(trimmed(family));
        if (generic == L"sans-serif" || generic == L"system-ui") {
            for (const auto* value : {L"Microsoft YaHei UI", L"Yu Gothic UI",
                    L"Meiryo", L"Segoe UI"}) append_unique(result, value);
        } else if (generic == L"serif") {
            for (const auto* value : {L"Yu Mincho", L"MS Mincho", L"SimSun",
                    L"Times New Roman"}) append_unique(result, value);
        } else if (generic == L"monospace") {
            for (const auto* value : {L"Cascadia Mono", L"Consolas",
                    L"MS Gothic", L"Courier New"}) append_unique(result, value);
        } else append_unique(result, family);
    }
    for (const auto* value : {L"Microsoft YaHei UI", L"Yu Gothic UI", L"Meiryo",
            L"Segoe UI", L"Arial"}) append_unique(result, value);
    return result;
}

std::wstring request_key(const std::vector<std::wstring>& families, bool bold) {
    std::wstring key = bold ? L"b" : L"n";
    for (const auto& name : families) {
        key += std::to_wstring(name.size()); key += L':'; key += name;
    }
    return key;
}

void append_key_name(std::string& key, std::wstring_view name) {
    // Length-prefixed UTF-16 code units avoid locale conversions and separators
    // inside user names colliding with a different fallback chain.
    key += std::to_string(name.size()); key += ':';
    for (const auto ch : name) {
        key.push_back(static_cast<char>(ch & 0xff));
        key.push_back(static_cast<char>((ch >> 8) & 0xff));
    }
}

void append_face_identity(std::string& key, IDWriteFontFace* face) {
    key += "|face:" + std::to_string(face->GetIndex());
    UINT32 count{};
    if (SUCCEEDED(face->GetFiles(&count, nullptr))) {
        std::vector<IDWriteFontFile*> files(count);
        if (SUCCEEDED(face->GetFiles(&count, files.data()))) {
            for (auto* file : files) {
                const void* data{}; UINT32 size{};
                IDWriteFontFileLoader* loader{};
                if (SUCCEEDED(file->GetLoader(&loader))
                        && SUCCEEDED(file->GetReferenceKey(&data, &size))) {
                    key += '|' + std::to_string(reinterpret_cast<std::uintptr_t>(loader));
                    key += ':' + std::to_string(size) + ':';
                    key.append(static_cast<const char*>(data), size);
                }
                release(loader);
            }
        }
        for (auto*& file : files) release(file);
    }
    IDWriteFontFace5* face5{};
    if (SUCCEEDED(face->QueryInterface(IID_PPV_ARGS(&face5)))) {
        const auto axis_count = face5->GetFontAxisValueCount();
        std::vector<DWRITE_FONT_AXIS_VALUE> axes(axis_count);
        if (SUCCEEDED(face5->GetFontAxisValues(axes.data(), axis_count))) {
            for (const auto& axis : axes) {
                key += "|axis:" + std::to_string(static_cast<UINT32>(axis.axisTag));
                key += ':' + std::to_string(std::bit_cast<std::uint32_t>(axis.value));
            }
        }
        release(face5);
    }
}

HRESULT native_bold_face(IDWriteFactory* factory, IDWriteFontFace* base,
    IDWriteFontFace** result) {
    const auto simulations = static_cast<DWRITE_FONT_SIMULATIONS>(
        base->GetSimulations() | DWRITE_FONT_SIMULATIONS_BOLD);
    IDWriteFontFace5* face5{};
    auto hr = base->QueryInterface(IID_PPV_ARGS(&face5));
    if (SUCCEEDED(hr)) {
        const auto count = face5->GetFontAxisValueCount();
        std::vector<DWRITE_FONT_AXIS_VALUE> axes(count);
        IDWriteFontResource* resource{};
        IDWriteFontFace5* simulated{};
        hr = face5->GetFontAxisValues(axes.data(), count);
        if (SUCCEEDED(hr)) hr = face5->GetFontResource(&resource);
        if (SUCCEEDED(hr)) hr = resource->CreateFontFace(simulations,
            axes.data(), count, &simulated);
        if (SUCCEEDED(hr)) { *result = simulated; simulated = nullptr; }
        release(simulated); release(resource); release(face5);
        return hr;
    }
    if (hr != E_NOINTERFACE) return hr;

    // Only static fonts can be reconstructed without the axis API.
    const void* table{}; UINT32 table_size{}; void* context{}; BOOL variable{};
    hr = base->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('f', 'v', 'a', 'r'),
        &table, &table_size, &context, &variable);
    if (context) base->ReleaseFontTable(context);
    if (FAILED(hr)) return hr;
    if (variable) return E_NOINTERFACE;
    UINT32 count{};
    hr = base->GetFiles(&count, nullptr);
    if (FAILED(hr)) return hr;
    std::vector<IDWriteFontFile*> files(count);
    hr = base->GetFiles(&count, files.data());
    if (SUCCEEDED(hr)) hr = factory->CreateFontFace(base->GetType(), count,
        files.data(), base->GetIndex(), simulations, result);
    for (auto*& file : files) release(file);
    return hr;
}

} // namespace

ResolvedFont::~ResolvedFont() { release(fallback_); }
FontResolver::OutlineFace::~OutlineFace() { release(outline); release(base); }
FontResolver::~FontResolver() { reset(); }

bool FontResolver::initialize(IDWriteFactory* factory,
    IDWriteFontCollection* collection) {
    reset();
    if (!factory) return false;
    factory_ = factory; factory_->AddRef();
    factory_->QueryInterface(IID_PPV_ARGS(&factory2_));
    if (collection) { collection_ = collection; collection_->AddRef(); }
    else factory_->GetSystemFontCollection(&collection_);
    return collection_ != nullptr;
}

void FontResolver::reset() {
    outline_faces_.clear();
    cache_.clear();
    release(collection_); release(factory2_); release(factory_);
}

const ResolvedFont& FontResolver::resolve(
    const std::vector<std::wstring>& families, bool additional_bold) {
    auto key = request_key(families, additional_bold);
    if (const auto it = cache_.find(key); it != cache_.end()) return *it->second;
    auto resolved = std::make_unique<ResolvedFont>();
    auto& result = *resolved;
    const auto expanded = expanded_families(families);
    const auto original = families.empty() ? expanded.front() : families.front();
    const auto requested = trimmed(original);
    result.request = parse_font_request(original);
    IDWriteFontFamily* primary = find_family(collection_, requested);
    IDWriteFont* base_font{};
    bool explicit_face = result.request.has_explicit_weight_suffix;
    if (primary) {
        // Exact family names win over all suffix heuristics (e.g. "Foo Light"
        // can be an independent, real family rather than a style of "Foo").
        result.original_family_exists = true;
        result.request.family_name = requested;
        result.request.base_weight = 400;
        result.request.has_explicit_weight_suffix = false;
        explicit_face = false;
    } else if (explicit_face) {
        primary = find_family(collection_, result.request.family_name);
    }
    if (!primary) {
        base_font = find_full_face(collection_, requested);
        if (base_font) {
            base_font->GetFontFamily(&primary);
            result.request.base_weight = base_font->GetWeight();
            result.style = base_font->GetStyle();
            result.stretch = base_font->GetStretch();
            result.full_face_match = true;
            explicit_face = true;
        } else {
            // Preserve the original request if the stripped name is unusable;
            // a missing Heavy name still carries its weight into fallback.
            result.request.family_name = requested;
        }
    }
    if (primary) {
        result.family_name = family_name(primary);
        result.request.family_name = result.family_name;
        if (!base_font) base_font = match(primary, result.request.base_weight,
            result.style, result.stretch);
        if (base_font) result.matched_base_weight = base_font->GetWeight();
    }
    result.explicit_face = explicit_face;
    result.additional_bold = additional_bold;
    result.layout_weight = requested_layout_weight(result.request.base_weight,
        additional_bold, explicit_face);

    if (primary) append_unique(result.fallback_families, result.family_name);
    for (const auto& candidate : expanded) {
        const auto name = trimmed(candidate);
        auto* family = find_family(collection_, name);
        if (!family) {
            const auto parsed = parse_font_request(name);
            if (parsed.has_explicit_weight_suffix)
                family = find_family(collection_, parsed.family_name);
        }
        if (!family) {
            if (auto* face = find_full_face(collection_, name)) {
                face->GetFontFamily(&family); release(face);
            }
        }
        if (!family) continue;
        const auto canonical = family_name(family);
        append_unique(result.fallback_families, canonical);
        if (!primary) {
            primary = family; primary->AddRef();
            result.family_name = canonical;
        }
        release(family);
    }
    if (!base_font && primary) {
        base_font = match(primary, result.request.base_weight, result.style, result.stretch);
        if (base_font) result.matched_base_weight = base_font->GetWeight();
    }
    IDWriteFontFace* layout_face{};
    if (auto* selected = match(primary, result.layout_weight,
            result.style, result.stretch)) {
        result.matched_weight = selected->GetWeight();
        result.layout_simulations = selected->GetSimulations();
        selected->CreateFontFace(&layout_face);
        release(selected);
    }
    result.draw_bold_simulation = explicit_face && additional_bold
        && result.matched_weight > 0 && result.matched_weight < 600
        && !(result.layout_simulations & DWRITE_FONT_SIMULATIONS_BOLD);

    // Let DirectWrite select each missing glyph with the SAME layout weight,
    // using the requested family chain before the normal Windows fallback.
    if (factory2_ && !result.fallback_families.empty()) {
        IDWriteFontFallbackBuilder* builder{};
        IDWriteFontFallback* system{};
        if (SUCCEEDED(factory2_->CreateFontFallbackBuilder(&builder))) {
            std::vector<const wchar_t*> names;
            for (const auto& name : result.fallback_families)
                names.push_back(name.c_str());
            const DWRITE_UNICODE_RANGE all{0, 0x10ffff};
            const auto mapping = builder->AddMapping(&all, 1, names.data(),
                static_cast<UINT32>(names.size()), collection_, nullptr,
                nullptr, 1.0F);
            if (SUCCEEDED(mapping)) {
                if (SUCCEEDED(factory2_->GetSystemFontFallback(&system)))
                    builder->AddMappings(system);
                builder->CreateFontFallback(&result.fallback_);
            }
            release(system); release(builder);
        }
    }
    result.cache_key = "font|";
    append_key_name(result.cache_key, result.request.requested_name);
    append_key_name(result.cache_key, result.family_name);
    for (const auto& name : result.fallback_families)
        append_key_name(result.cache_key, name);
    for (const int value : {result.request.base_weight, result.layout_weight,
            result.matched_base_weight, result.matched_weight,
            static_cast<int>(result.style), static_cast<int>(result.stretch),
            static_cast<int>(result.layout_simulations),
            static_cast<int>(result.explicit_face), static_cast<int>(additional_bold),
            static_cast<int>(result.draw_bold_simulation)}) {
        result.cache_key += '|' + std::to_string(value);
    }
    if (layout_face) append_face_identity(result.cache_key, layout_face);
    release(layout_face);
    release(base_font); release(primary);
    return *cache_.emplace(std::move(key), std::move(resolved)).first->second;
}

IDWriteTextFormat* FontResolver::create_format(
    const ResolvedFont& font, float size) const {
    if (!factory_) return nullptr;
    IDWriteTextFormat* format{};
    if (FAILED(factory_->CreateTextFormat(font.family_name.c_str(), collection_,
            static_cast<DWRITE_FONT_WEIGHT>(font.layout_weight), font.style,
            font.stretch, size, L"zh-CN", &format))) return nullptr;
    if (font.fallback_) {
        IDWriteTextFormat1* format1{};
        if (SUCCEEDED(format->QueryInterface(IID_PPV_ARGS(&format1)))) {
            format1->SetFontFallback(font.fallback_); release(format1);
        }
    }
    return format;
}

HRESULT FontResolver::outline_face(const ResolvedFont& font,
    IDWriteFontFace* base, IDWriteFontFace** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (!base || !factory_) return E_INVALIDARG;
    if (!font.explicit_face || !font.additional_bold
            || (base->GetSimulations() & DWRITE_FONT_SIMULATIONS_BOLD)) {
        base->AddRef(); *result = base; return S_OK;
    }
    if (const auto found = outline_faces_.find(base); found != outline_faces_.end()) {
        found->second->outline->AddRef();
        *result = found->second->outline;
        return S_OK;
    }
    int weight{};
    IDWriteFontFace3* face3{};
    if (SUCCEEDED(base->QueryInterface(IID_PPV_ARGS(&face3)))) {
        weight = face3->GetWeight(); release(face3);
    } else {
        IDWriteFont* matched{};
        const auto hr = collection_->GetFontFromFontFace(base, &matched);
        if (FAILED(hr)) return hr;
        weight = matched->GetWeight(); release(matched);
    }
    auto cached = std::make_unique<OutlineFace>();
    cached->base = base; base->AddRef();
    if (weight < 600) {
        const auto hr = native_bold_face(factory_, base, &cached->outline);
        if (FAILED(hr)) return hr;
    } else {
        cached->outline = base; base->AddRef();
    }
    cached->outline->AddRef(); *result = cached->outline;
    outline_faces_.emplace(base, std::move(cached));
    return S_OK;
}

HRESULT FontResolver::glyph_run_outline(const ResolvedFont& font,
    const DWRITE_GLYPH_RUN& run, IDWriteGeometrySink* sink) {
    IDWriteFontFace* face{};
    auto hr = outline_face(font, run.fontFace, &face);
    if (SUCCEEDED(hr)) hr = face->GetGlyphRunOutline(run.fontEmSize,
        run.glyphIndices, run.glyphAdvances, run.glyphOffsets, run.glyphCount,
        run.isSideways, (run.bidiLevel & 1U) != 0, sink);
    release(face);
    return hr;
}

} // namespace kirakara::show::win32::detail
