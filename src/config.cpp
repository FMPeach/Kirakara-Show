#include "kirakara/show/config.hpp"

#include "json_value.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace kirakara::show {
namespace {

using detail::json::Value;

// ── Prelude（时间前补）背景图读取开关 ─────────────────────────────
// 前补功能因追帧问题被禁用（旧实现以 if(0) 注释后被重构清理）。
// krl 文件里仍保留 backgroundImage 的 base64（单曲可达数百 KB），
// 当前禁用期解析时跳过该字段，避免每首歌为无用的负载解析/分配内存。
// 待前补功能补全并解决追帧问题后：把 kReadPreludeBackgroundImage 置
// true，并在此处补充判断读取时机的逻辑。
constexpr bool kReadPreludeBackgroundImage = false;

std::string read_text(std::wstring_view path) {
    std::ifstream stream(std::filesystem::path(std::wstring(path)),
        std::ios::binary);
    if (!stream) return {};
    return {std::istreambuf_iterator<char>{stream},
        std::istreambuf_iterator<char>{}};
}

std::wstring ascii_to_wide(std::string_view value) {
    return {value.begin(), value.end()};
}

std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) return {};
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return ascii_to_wide(value);
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count);
    return result;
}

void trim_ascii(std::string& value) {
    while (!value.empty()
            && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty()
            && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
}

std::vector<std::wstring> font_family_list(std::string value) {
    std::vector<std::wstring> result;
    std::string current;
    char quote = '\0';
    bool escaped = false;
    for (const auto ch : value) {
        if (escaped) {
            current.push_back(ch);
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (quote != '\0') {
            if (ch == quote) quote = '\0';
            else current.push_back(ch);
        } else if (ch == '\'' || ch == '"') {
            quote = ch;
        } else if (ch == ',') {
            trim_ascii(current);
            if (!current.empty()) result.push_back(utf8_to_wide(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    trim_ascii(current);
    if (!current.empty()) result.push_back(utf8_to_wide(current));
    if (result.empty()) result.push_back(L"MotoyaLMaru W3 Mono");
    return result;
}

void set_font_families(AppConfig& config, const std::string& value) {
    config.font_families = font_family_list(value);
    config.font_family = config.font_families.empty()
        ? L"MotoyaLMaru W3 Mono" : config.font_families.front();
}

void ensure_font_family_chain(AppConfig& config) {
    if (config.font_families.empty()) {
        config.font_families.push_back(config.font_family.empty()
            ? L"MotoyaLMaru W3 Mono" : config.font_family);
    }
    if (config.font_family.empty()) config.font_family = config.font_families.front();
}

const Value* member(const Value& object, std::string_view key) {
    return object.find(key);
}

double number_value(const Value& object, std::string_view key,
        double fallback) {
    const auto* value = member(object, key);
    const auto* number = value ? value->number() : nullptr;
    return number && std::isfinite(*number) ? *number : fallback;
}

bool bool_value(const Value& object, std::string_view key, bool fallback) {
    const auto* value = member(object, key);
    if (!value) return fallback;
    if (const auto* boolean = value->boolean()) return *boolean;
    if (const auto* number = value->number()) return *number != 0.0;
    return fallback;
}

std::string string_value(const Value& object, std::string_view key,
        std::string fallback = {}) {
    const auto* value = member(object, key);
    const auto* string = value ? value->string() : nullptr;
    return string ? *string : std::move(fallback);
}

int hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::optional<PackedColor> parse_packed_color(std::string_view value) {
    if (value.size() != 7 || value[0] != '#') return std::nullopt;
    std::array<int, 6> digits{};
    for (std::size_t index = 0; index < digits.size(); ++index) {
        digits[index] = hex_digit(value[index + 1]);
        if (digits[index] < 0) return std::nullopt;
    }
    return packed_rgb(
        static_cast<std::uint8_t>(digits[0] * 16 + digits[1]),
        static_cast<std::uint8_t>(digits[2] * 16 + digits[3]),
        static_cast<std::uint8_t>(digits[4] * 16 + digits[5]));
}

PackedColor color_value(const Value& object, std::string_view key,
        PackedColor fallback) {
    const auto raw = string_value(object, key);
    return raw.empty() ? fallback : parse_packed_color(raw).value_or(fallback);
}

Color color_from_packed(PackedColor value) {
    return {
        static_cast<float>(value & 0xffU) / 255.0F,
        static_cast<float>((value >> 8U) & 0xffU) / 255.0F,
        static_cast<float>((value >> 16U) & 0xffU) / 255.0F,
        1.0F,
    };
}

int base64_value(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

std::vector<std::uint8_t> decode_base64(std::string_view input) {
    std::vector<std::uint8_t> result;
    result.reserve(input.size() * 3 / 4 + 1);
    int buffer = 0;
    int bits = 0;
    for (auto ch : input) {
        if (ch == '=') break;
        const int value = base64_value(static_cast<unsigned char>(ch));
        if (value < 0) continue;
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xff));
        }
    }
    return result;
}

SongTitleAlign title_align_value(const Value& object,
        SongTitleAlign fallback) {
    const auto align = string_value(object, "align");
    if (align == "left") return SongTitleAlign::left;
    if (align == "center-left") return SongTitleAlign::center_left;
    if (align == "center") return SongTitleAlign::center;
    if (align == "center-right") return SongTitleAlign::center_right;
    if (align == "right") return SongTitleAlign::right;
    return fallback;
}

SongTitleStyle normalize_title_style(const Value* raw,
        SongTitleStyle defaults) {
    if (!raw || !raw->object()) return defaults;
    if (const auto family = string_value(*raw, "fontFamily"); !family.empty()) {
        defaults.font_families = font_family_list(family);
        defaults.font_family = defaults.font_families.front();
    }
    defaults.font_size = static_cast<float>(std::max(1.0,
        number_value(*raw, "fontSize", defaults.font_size)));
    defaults.font_bold = bool_value(*raw, "fontBold", defaults.font_bold);
    defaults.letter_spacing = static_cast<float>(number_value(
        *raw, "letterSpacing", defaults.letter_spacing));
    defaults.line_spacing = static_cast<float>(number_value(
        *raw, "lineSpacing", defaults.line_spacing));
    defaults.x = static_cast<float>(number_value(*raw, "x", defaults.x));
    defaults.y = static_cast<float>(number_value(*raw, "y", defaults.y));
    defaults.align = title_align_value(*raw, defaults.align);
    defaults.color = color_value(*raw, "color", defaults.color);
    defaults.stroke_color = color_value(
        *raw, "strokeColor", defaults.stroke_color);
    defaults.stroke_width = static_cast<float>(std::max(0.0,
        number_value(*raw, "strokeWidth", defaults.stroke_width)));
    return defaults;
}

std::vector<std::string> normalize_title_lines(const Value& source) {
    if (const auto value = string_value(source, "value"); !value.empty()) {
        return {value};
    }

    std::vector<std::string> result;
    if (const auto* lines = member(source, "lines");
            lines && lines->array()) {
        for (const auto& line : *lines->array()) {
            if (const auto* text = line.string(); text && !text->empty()) {
                result.push_back(*text);
            }
        }
    }
    if (!result.empty()) return result;

    if (const auto* rows = member(source, "rows");
            rows && rows->array()) {
        for (const auto& row : *rows->array()) {
            if (!row.object()) continue;
            auto line = string_value(row, "before")
                + string_value(row, "separator")
                + string_value(row, "after");
            if (!line.empty()) result.push_back(std::move(line));
        }
    }
    return result;
}

SongTitleConfig normalize_song_title(const Value* raw) {
    auto result = default_song_title_config();
    if (!raw || !raw->object()) return result;

    result.enabled = bool_value(*raw, "enabled", result.enabled);
    result.duration = std::clamp(number_value(*raw, "durationSec",
        result.duration), 3.0, 10.0);
    const auto* text_fade = member(*raw, "textFade");
    if (!text_fade) text_fade = member(*raw, "textFadeEnabled");
    if (text_fade) {
        if (const auto* value = text_fade->boolean()) {
            result.text_fade = *value;
        } else if (text_fade->object()) {
            result.text_fade = bool_value(*text_fade, "enabled", true);
        }
    }

    const auto* groups_value = member(*raw, "groups");
    const auto* groups = groups_value ? groups_value->array() : nullptr;
    if (!groups) return result;

    result.blocks.clear();
    for (const auto& source : *groups) {
        if (!source.object()) continue;
        SongTitleBlock block;
        block.lines = normalize_title_lines(source);
        block.style = normalize_title_style(
            member(source, "style"), SongTitleStyle{});
        if (!block.lines.empty()) {
            result.blocks.push_back(std::move(block));
        }
    }
    return result;
}

bool apply_json(std::string_view text, AppConfig& config) {
    const auto root = kReadPreludeBackgroundImage
        ? detail::json::parse(text)
        : detail::json::parse(
            text, std::vector<std::string>{"backgroundImage"});
    if (!root || !root->object()) return false;

    if (const auto family = string_value(*root, "fontFamily"); !family.empty()) {
        set_font_families(config, family);
    } else {
        ensure_font_family_chain(config);
    }

    auto& style = config.style;
    style.font_bold = bool_value(*root, "fontBold", style.font_bold);
    style.ruby_bold = bool_value(*root, "rubyBold", style.ruby_bold);
    style.ruby2_bold = bool_value(*root, "ruby2Bold", style.ruby2_bold);
    style.ruby_isolate = bool_value(
        *root, "rubyIsolateEnabled", style.ruby_isolate);
    style.font_size = static_cast<float>(number_value(
        *root, "fontSize", style.font_size));
    style.ruby_size = static_cast<float>(number_value(
        *root, "rubySize", style.ruby_size));
    style.ruby2_size = static_cast<float>(number_value(
        *root, "ruby2Size", style.ruby2_size));
    style.ruby_above_offset = static_cast<float>(number_value(
        *root, "rubyOffset", style.ruby_above_offset));
    style.ruby_below_offset = static_cast<float>(number_value(
        *root, "ruby2Offset", style.ruby_below_offset));
    style.letter_spacing = static_cast<float>(number_value(
        *root, "letterSpacing", style.letter_spacing));
    style.ruby_letter_spacing = static_cast<float>(number_value(
        *root, "rubyLetterSpacing", style.ruby_letter_spacing));
    style.ruby2_letter_spacing = static_cast<float>(number_value(
        *root, "ruby2LetterSpacing", style.ruby2_letter_spacing));
    style.stroke_width = static_cast<float>(number_value(
        *root, "strokeWidth", style.stroke_width));
    style.ruby_stroke_width = static_cast<float>(number_value(
        *root, "rubyStrokeWidth", style.ruby_stroke_width));
    style.ruby2_stroke_width = static_cast<float>(number_value(
        *root, "ruby2StrokeWidth", style.ruby2_stroke_width));
    style.before = color_value(*root, "colorBefore", style.before);
    style.after = color_value(*root, "colorAfter", style.after);
    style.stroke_before = color_value(
        *root, "strokeColorBefore", style.stroke_before);
    style.stroke_after = color_value(
        *root, "strokeColorAfter", style.stroke_after);
    style.background = color_value(*root, "bgColor", style.background);
    style.background_image_opacity = static_cast<float>(number_value(
        *root, "bgImageOpacity", style.background_image_opacity));
    style.role_colors = true;
    style.show_role_labels = bool_value(
        *root, "showRoleLabels", style.show_role_labels);
    style.role_label_prefix = string_value(*root, "roleLabelPrefix",
        style.role_label_prefix);
    style.role_label_separator = string_value(*root, "roleLabelSeparator",
        style.role_label_separator);
    style.role_label_suffix = string_value(*root, "roleLabelSuffix",
        style.role_label_suffix);

    if (const auto* profiles_value = member(*root, "characterProfiles");
            profiles_value && profiles_value->object()) {
        style.character_profiles.clear();
        for (const auto& [role, raw] : *profiles_value->object()) {
            if (!raw.object()) continue;
            auto& profile = style.character_profiles[role];
            profile.color_before = color_value(raw, "colorBefore", {});
            profile.color_after = color_value(raw, "colorAfter", {});
            profile.stroke_before = color_value(raw, "strokeColorBefore", {});
            profile.stroke_after = color_value(raw, "strokeColorAfter", {});
            profile.stroke_width = static_cast<float>(number_value(
                raw, "strokeWidth", 0.0));
            profile.show_label = bool_value(raw, "showLabel", false);
            profile.image_mode = bool_value(raw, "imageMode", true);
            profile.display_name = string_value(raw, "displayName");
            profile.display_color = color_value(raw, "displayColor", {});
            profile.label_stroke_color = color_value(
                raw, "labelStrokeColor", {});
            profile.label_scale = static_cast<float>(number_value(
                raw, "labelScale", 100.0));
            profile.label_margin_left = static_cast<float>(number_value(
                raw, "labelMarginLeft", 0.0));
            profile.label_margin_right = static_cast<float>(number_value(
                raw, "labelMarginRight", 0.0));
            profile.label_offset_y = static_cast<float>(number_value(
                raw, "imageOffsetY", 0.0));
            if (const auto* image = member(raw, "image")) {
                if (image->string()) {
                    profile.image_path = utf8_to_wide(*image->string());
                } else if (image->object()) {
                    const auto b64 = string_value(*image, "base64");
                    if (!b64.empty()) {
                        profile.image_data = decode_base64(b64);
                    }
                }
            }
        }
    }

    style.line1_x = static_cast<float>(number_value(
        *root, "line1X", style.line1_x));
    style.line1_y = static_cast<float>(number_value(
        *root, "line1Y", style.line1_y));
    style.line2_right = static_cast<float>(number_value(
        *root, "line2Right", style.line2_right));
    style.line2_y = static_cast<float>(number_value(
        *root, "line2Y", style.line2_y));

    auto& engine = config.engine;
    const auto fade_enabled = bool_value(*root, "fadeEnabled",
        engine.fade_mode != FadeMode::disabled);
    const auto fade_paragraph_only = bool_value(
        *root, "fadeParagraphOnly", true);
    engine.fade_mode = !fade_enabled ? FadeMode::disabled
        : fade_paragraph_only ? FadeMode::paragraph_edges
                              : FadeMode::every_line;
    engine.fade_duration = number_value(*root, "fadeDurationMs",
        engine.fade_duration * 1000.0) / 1000.0;
    engine.indicator.enabled = bool_value(
        *root, "indicatorEnabled", engine.indicator.enabled);
    engine.indicator.duration = number_value(
        *root, "indicatorDuration", engine.indicator.duration);
    engine.indicator.fade_ratio = static_cast<float>(number_value(
        *root, "indicatorFadeRatio", engine.indicator.fade_ratio));
    engine.indicator.size = static_cast<float>(number_value(
        *root, "indicatorSize", engine.indicator.size));
    engine.indicator.spacing = static_cast<float>(number_value(
        *root, "indicatorSpacing", engine.indicator.spacing));
    engine.indicator.stroke_width = static_cast<float>(number_value(
        *root, "indicatorStrokeWidth", engine.indicator.stroke_width));
    engine.indicator.offset_x = static_cast<float>(number_value(
        *root, "indicatorOffsetX", engine.indicator.offset_x));
    engine.indicator.offset_y = static_cast<float>(number_value(
        *root, "indicatorOffsetY", engine.indicator.offset_y));
    engine.indicator.fill = color_from_packed(color_value(
        *root, "indicatorFillColor", packed_rgb(255, 255, 255)));
    engine.indicator.stroke = color_from_packed(color_value(
        *root, "indicatorStrokeColor", packed_rgb(0, 0, 0)));

    if (const auto* song_title = member(*root, "songTitle")) {
        config.song_title = normalize_song_title(song_title);
    }
    return true;
}

} // namespace

AppConfig default_app_config() {
    AppConfig config;
    static_cast<void>(apply_json(R"({
        "fontFamily": "'MotoyaLMaru W3 Mono', monospace",
        "fontBold": true,
        "rubyBold": true,
        "ruby2Bold": false,
        "rubyIsolateEnabled": false,
        "fontSize": 62,
        "rubySize": 26,
        "ruby2Size": 20,
        "rubyOffset": 1,
        "ruby2Offset": 4,
        "letterSpacing": 12,
        "rubyLetterSpacing": 5,
        "ruby2LetterSpacing": 4,
        "strokeWidth": 4,
        "rubyStrokeWidth": 3,
        "ruby2StrokeWidth": 3,
        "colorBefore": "#ffffff",
        "colorAfter": "#0000a5",
        "strokeColorBefore": "#000000",
        "strokeColorAfter": "#ffffff",
        "bgColor": "#095500",
        "bgImageOpacity": 1.0,
        "showRoleLabels": false,
        "fadeEnabled": true,
        "fadeParagraphOnly": true,
        "fadeDurationMs": 666,
        "indicatorEnabled": true,
        "indicatorDuration": 3,
        "indicatorFadeRatio": 0,
        "indicatorSize": 34,
        "indicatorSpacing": 12,
        "indicatorStrokeWidth": 3,
        "indicatorFillColor": "#ffffff",
        "indicatorStrokeColor": "#000000",
        "indicatorOffsetX": 0,
        "indicatorOffsetY": 8,
        "line1X": 128,
        "line1Y": 450,
        "line2Right": 128,
        "line2Y": 566
    })", config));
    return config;
}

bool load_app_config(std::wstring_view path, AppConfig& config) {
    const auto text = read_text(path);
    return !text.empty() && apply_json(text, config);
}

bool load_app_config_json(std::string_view json, AppConfig& config) {
    return !json.empty() && apply_json(json, config);
}

} // namespace kirakara::show
