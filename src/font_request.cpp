#include "kirakara/show/font_request.hpp"

#include <algorithm>

namespace kirakara::show {
namespace {

bool space(wchar_t ch) noexcept {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

std::wstring_view trimmed(std::wstring_view value) noexcept {
    while (!value.empty() && space(value.front())) value.remove_prefix(1);
    while (!value.empty() && space(value.back())) value.remove_suffix(1);
    return value;
}

bool equal_ascii(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](wchar_t ch) {
            return ch >= L'A' && ch <= L'Z' ? ch - L'A' + L'a' : ch;
        };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

} // namespace

FontRequest parse_font_request(std::wstring_view name) {
    FontRequest result;
    result.requested_name = name;
    name = trimmed(name);
    result.family_name = name;
    const auto split = name.find_last_of(L" \t\r\n");
    if (split == std::wstring_view::npos) return result;
    const auto family = trimmed(name.substr(0, split));
    if (family.empty()) return result;
    const auto suffix = name.substr(split + 1);
    struct Suffix { std::wstring_view name; int weight; };
    static constexpr Suffix suffixes[]{
        {L"Thin", 100}, {L"ExtraLight", 200}, {L"UltraLight", 200},
        {L"Light", 300}, {L"Regular", 400}, {L"Medium", 500},
        {L"DemiBold", 600}, {L"SemiBold", 600}, {L"ExtraBold", 800},
        {L"UltraBold", 800}, {L"Black", 900}, {L"Heavy", 900},
    };
    // Bold deliberately is not a heuristic suffix. The Windows resolver may
    // recognize it as a real full face name using DirectWrite font metadata.
    for (const auto& entry : suffixes) {
        if (!equal_ascii(suffix, entry.name)) continue;
        result.family_name = family;
        result.base_weight = entry.weight;
        result.has_explicit_weight_suffix = true;
        break;
    }
    return result;
}

int requested_layout_weight(int base_weight, bool additional_bold,
    bool explicit_face) noexcept {
    const int base = std::clamp(base_weight, 100, 900);
    return !explicit_face && additional_bold ? 700 : base;
}

} // namespace kirakara::show
