#pragma once

#include <string>
#include <string_view>

namespace kirakara::show {

// A name hint, not proof that a font is installed. Platform resolution must
// prefer an exact family and verify a stripped family before using it.
struct FontRequest {
    std::wstring requested_name;
    std::wstring family_name;
    int base_weight{400};
    bool has_explicit_weight_suffix{};
};

[[nodiscard]] FontRequest parse_font_request(std::wstring_view name);

// Numeric approximation of an additional Bold request, never glyph emboldening.
// Without a named face, retain the existing normal=400 / bold=700 contract.
// matched_base_weight lets the platform avoid adding Bold to an already-bold
// match (e.g. a family whose closest match for Medium is its real SemiBold face).
[[nodiscard]] int requested_font_weight(int base_weight, bool additional_bold,
    bool explicit_face, int matched_base_weight = 0) noexcept;

} // namespace kirakara::show
