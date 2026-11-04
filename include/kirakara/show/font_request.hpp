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

// Named faces retain their base weight for layout. Additional Bold is a separate
// drawing request. Plain families use the normal=400 / bold=700 matching rule.
[[nodiscard]] int requested_layout_weight(int base_weight, bool additional_bold,
    bool explicit_face) noexcept;

} // namespace kirakara::show
