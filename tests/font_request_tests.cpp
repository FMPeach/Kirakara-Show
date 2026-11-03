#include "kirakara/show/font_request.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace kirakara::show;

namespace {
void expect(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
}

int main() {
    struct Case { const wchar_t* suffix; int base; int bold; };
    const Case cases[]{
        {L"Thin", 100, 350}, {L"ExtraLight", 200, 425},
        {L"UltraLight", 200, 425}, {L"Light", 300, 500},
        {L"Regular", 400, 700}, {L"Medium", 500, 800},
        {L"DemiBold", 600, 600}, {L"SemiBold", 600, 600},
        {L"ExtraBold", 800, 800}, {L"UltraBold", 800, 800},
        {L"Black", 900, 900}, {L"Heavy", 900, 900},
    };
    for (const auto& test : cases) {
        const auto name = std::wstring{L"Noto Sans JP "} + test.suffix;
        const auto parsed = parse_font_request(name);
        expect(parsed.requested_name == name, "original requested name retained");
        expect(parsed.family_name == L"Noto Sans JP", "weight suffix family");
        expect(parsed.base_weight == test.base, "weight suffix numeric value");
        expect(parsed.has_explicit_weight_suffix, "explicit suffix flag");
        expect(requested_font_weight(parsed.base_weight, false, true) == test.base,
            "named base weight without additional Bold");
        expect(requested_font_weight(parsed.base_weight, true, true) == test.bold,
            "named face numeric Bold approximation");
    }
    const auto plain = parse_font_request(L"Noto Sans JP");
    expect(plain.family_name == L"Noto Sans JP" && plain.base_weight == 400
        && !plain.has_explicit_weight_suffix, "plain family remains Regular");
    expect(requested_font_weight(400, false, false) == 400, "legacy normal");
    expect(requested_font_weight(400, true, false) == 700, "legacy bold");
    const auto mixed = parse_font_request(L"  Noto Sans JP\tSeMiBoLd  ");
    expect(mixed.family_name == L"Noto Sans JP" && mixed.base_weight == 600,
        "case insensitive suffix and surrounding whitespace");
    expect(mixed.requested_name == L"  Noto Sans JP\tSeMiBoLd  ",
        "whitespace preserved in requested diagnostic name");
    for (const auto* value : {L"Light Family", L"Noto Sans JPHeavy", L"Heavy",
            L"Noto Sans JP Light Extra", L"Noto Sans JP Bold", L"", L"   "}) {
        expect(!parse_font_request(value).has_explicit_weight_suffix,
            "only a complete final non-Bold word is stripped");
    }
    expect(requested_font_weight(600, true, true, 700) == 600,
        "SemiBold already matching real Bold is not emboldened again");
    expect(requested_font_weight(500, true, true, 600) == 500,
        "Medium already matching real SemiBold is not emboldened again");
    expect(requested_font_weight(300, true, true, 400) == 700,
        "Regular-only family may request native Bold simulation");
    expect(requested_font_weight(700, true, true, 700) == 700,
        "confirmed full Bold face is not double-bolded");
    expect(requested_font_weight(900, true, true, 900) == 900,
        "Heavy remains Heavy");
    expect(requested_font_weight(1200, true, true) == 900, "upper bound");
    expect(requested_font_weight(-5, false, true) == 100, "lower bound");
    std::cout << "Font request parser and numeric weight tests passed.\n";
}
