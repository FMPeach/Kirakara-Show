#pragma once

#include "kirakara/show/font_request.hpp"

#include <dwrite_2.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace kirakara::show::win32::detail {

struct ResolvedFont {
    FontRequest request;
    std::wstring family_name;
    std::vector<std::wstring> fallback_families;
    int matched_base_weight{};
    int target_weight{400};
    int matched_weight{};
    DWRITE_FONT_STYLE style{DWRITE_FONT_STYLE_NORMAL};
    DWRITE_FONT_STRETCH stretch{DWRITE_FONT_STRETCH_NORMAL};
    DWRITE_FONT_SIMULATIONS simulations{DWRITE_FONT_SIMULATIONS_NONE};
    bool original_family_exists{};
    bool full_face_match{};
    std::string cache_key;

    ResolvedFont() = default;
    ~ResolvedFont();
    ResolvedFont(const ResolvedFont&) = delete;
    ResolvedFont& operator=(const ResolvedFont&) = delete;

private:
    friend class FontResolver;
    IDWriteFontFallback* fallback_{};
};

// Per-renderer, in-memory DirectWrite resolution. No font-file lookup, global
// installation, per-frame enumeration, or application-owned bold simulation.
class FontResolver {
public:
    FontResolver() = default;
    ~FontResolver();
    FontResolver(const FontResolver&) = delete;
    FontResolver& operator=(const FontResolver&) = delete;
    bool initialize(IDWriteFactory* factory,
        IDWriteFontCollection* collection = nullptr);
    void reset();
    const ResolvedFont& resolve(const std::vector<std::wstring>& families,
        bool additional_bold);
    IDWriteTextFormat* create_format(const ResolvedFont& font, float size) const;

private:
    IDWriteFactory* factory_{};
    IDWriteFactory2* factory2_{};
    IDWriteFontCollection* collection_{};
    std::unordered_map<std::wstring, std::unique_ptr<ResolvedFont>> cache_;
};

} // namespace kirakara::show::win32::detail
