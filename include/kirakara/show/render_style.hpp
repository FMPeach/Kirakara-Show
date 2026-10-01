#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kirakara::show {

using PackedColor = std::uint32_t; // 0x00BBGGRR, matching Win32 COLORREF.

constexpr PackedColor packed_rgb(std::uint8_t red, std::uint8_t green,
    std::uint8_t blue) noexcept {
    return static_cast<PackedColor>(red)
        | (static_cast<PackedColor>(green) << 8U)
        | (static_cast<PackedColor>(blue) << 16U);
}

// Fallback colour palette for roles the user hasn't configured.
inline constexpr std::array<PackedColor, 13> kRolePalette{{
    packed_rgb(180, 50, 50),   // crimson
    packed_rgb(50, 120, 220),  // blue
    packed_rgb(200, 150, 20),  // gold
    packed_rgb(30, 160, 60),   // green
    packed_rgb(200, 80, 160),  // magenta
    packed_rgb(20, 170, 150),  // teal
    packed_rgb(220, 100, 30),  // orange
    packed_rgb(100, 100, 200), // indigo
    packed_rgb(160, 160, 20),  // olive
    packed_rgb(60, 140, 180),  // steel
    packed_rgb(190, 60, 110),  // rose
    packed_rgb(140, 80, 200),  // purple
    packed_rgb(50, 170, 100),  // sea green
}};

/// Deterministic palette index from a role name string.
inline PackedColor palette_color(std::string_view name) noexcept {
    if (name.empty()) return packed_rgb(255, 255, 255);
    std::uint32_t h = 5381;
    for (auto c : name) h = ((h << 5) + h) + static_cast<unsigned char>(c);
    return kRolePalette[h % kRolePalette.size()];
}

struct TextPaint {
    PackedColor before{packed_rgb(255, 255, 255)};
    PackedColor after{packed_rgb(0, 0, 165)};
    PackedColor stroke_before{packed_rgb(0, 0, 0)};
    PackedColor stroke_after{packed_rgb(255, 255, 255)};
    float stroke_width{4.0F};
};

// Mirrors the DOM characterProfiles entry.  Every field is optional;
// missing values fall back to the global style, then to the palette.
struct CharacterProfile {
    // -- colours (per-role override of global before/after/stroke) --
    PackedColor color_before{};       // 0 means "use global / palette"
    PackedColor color_after{};
    PackedColor stroke_before{};
    PackedColor stroke_after{};
    float stroke_width{};             // 0 → global

    // -- label display --
    bool show_label{true};            // showRoleLabels must also be true
    bool image_mode{true};            // true=image, false=text
    std::string display_name;         // text label
    std::wstring image_path;          // image label (file path)
    std::vector<std::uint8_t> image_data; // image label (decoded base64 bytes)
    PackedColor display_color{};      // label colour
    PackedColor label_stroke_color{}; // label stroke
    float label_scale{100.0F};        // percent
    float label_margin_left{};
    float label_margin_right{};
    float label_offset_y{};
};

struct RenderStyle {
    float font_size{62.0F};
    bool font_bold{true};
    float ruby_size{26.0F};
    bool ruby_bold{true};
    float ruby2_size{20.0F};
    bool ruby2_bold{};
    float ruby_stroke_width{3.0F};
    float ruby2_stroke_width{3.0F};
    float stroke_width{4.0F};
    float letter_spacing{9.0F};
    PackedColor before{packed_rgb(255, 255, 255)};
    PackedColor after{packed_rgb(0, 0, 165)};
    PackedColor stroke_before{packed_rgb(0, 0, 0)};
    PackedColor stroke_after{packed_rgb(255, 255, 255)};
    PackedColor background{packed_rgb(9, 85, 0)};
    float background_image_opacity{1.0F};
    float line1_x{128.0F};
    float line1_y{430.0F};
    float line2_right{128.0F};
    float line2_y{566.0F};
    float line2_bottom{80.0F};
    float ruby_above_offset{4.0F};
    float ruby_below_offset{4.0F};
    float ruby_letter_spacing{5.0F};
    float ruby2_letter_spacing{4.0F};
    bool ruby_isolate{true};
    bool role_colors{true};
    bool show_role_labels{false};
    std::string role_label_prefix;
    std::string role_label_separator;
    std::string role_label_suffix;
    std::unordered_map<std::string, CharacterProfile> character_profiles;
};

} // namespace kirakara::show
