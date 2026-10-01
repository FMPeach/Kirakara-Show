#include "kirakara/show/lrc_parser.hpp"

#include "kirakara/show/timeline.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace kirakara::show {
namespace {

constexpr std::string_view kRoleOpen = "【@";
constexpr std::string_view kRoleLiteralOpen = "【";
constexpr std::string_view kRoleClose = "】";

struct Token {
    bool is_time{};
    Seconds time{};
    LyricChar character;
};

struct Segment {
    Seconds start{};
    std::optional<Seconds> end;
    std::vector<LyricChar> chars;
};

struct RubyDictionaryEntry {
    RubyTrack ruby;
    std::vector<std::pair<Seconds, Seconds>> ranges;
};

struct ReferenceGroup {
    std::size_t normalized_start{};
    std::size_t normalized_span{};
    RubyTrack above;
    RubyTrack below;
};

struct ReferenceLine {
    std::vector<std::string> normalized_chars;
    std::vector<ReferenceGroup> groups;
};

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

bool starts_with_at(std::string_view text, std::size_t position, std::string_view value) {
    return position <= text.size() && text.substr(position).starts_with(value);
}

std::size_t utf8_sequence_length(unsigned char lead) {
    if ((lead & 0x80U) == 0) return 1;
    if ((lead & 0xE0U) == 0xC0U) return 2;
    if ((lead & 0xF0U) == 0xE0U) return 3;
    if ((lead & 0xF8U) == 0xF0U) return 4;
    return 1;
}

std::vector<std::string> utf8_characters(std::string_view text) {
    std::vector<std::string> result;
    for (std::size_t i = 0; i < text.size();) {
        const auto count = std::min(utf8_sequence_length(
            static_cast<unsigned char>(text[i])), text.size() - i);
        result.emplace_back(text.substr(i, count));
        i += count;
    }
    return result;
}

bool is_spacing_character(std::string_view character) {
    return character == " " || character == "\t" || character == "\xE3\x80\x80";
}

std::vector<std::string> normalized_characters(std::string_view text) {
    auto chars = utf8_characters(text);
    std::erase_if(chars, [](const auto& character) {
        return is_spacing_character(character);
    });
    return chars;
}

std::optional<int> parse_integer(std::string_view value) {
    int result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return result;
}

std::optional<Seconds> parse_time_tag(std::string_view tag) {
    if (tag.size() < 5 || tag.front() != '[' || tag.back() != ']') {
        return std::nullopt;
    }
    tag.remove_prefix(1);
    tag.remove_suffix(1);
    const auto colon = tag.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    const auto separator = tag.find_first_of(".: ", colon + 1);
    const auto minutes = parse_integer(tag.substr(0, colon));
    const auto seconds_text = separator == std::string_view::npos
        ? tag.substr(colon + 1)
        : tag.substr(colon + 1, separator - colon - 1);
    const auto seconds = parse_integer(seconds_text);
    if (!minutes || !seconds) {
        return std::nullopt;
    }
    double fraction = 0.0;
    if (separator != std::string_view::npos) {
        auto fraction_text = tag.substr(separator + 1);
        const auto fraction_value = parse_integer(fraction_text);
        if (!fraction_value) {
            return std::nullopt;
        }
        fraction = static_cast<double>(*fraction_value);
        for (std::size_t i = 0; i < fraction_text.size(); ++i) {
            fraction /= 10.0;
        }
    }
    return static_cast<double>(*minutes * 60 + *seconds) + fraction;
}

std::vector<std::string> inline_ruby_base_characters(std::string_view text) {
    std::vector<std::string> result;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '[') {
            const auto close = text.find(']', i + 1);
            if (close != std::string_view::npos
                    && parse_time_tag(text.substr(i, close - i + 1))) {
                i = close + 1;
                continue;
            }
        }
        const auto count = std::min(utf8_sequence_length(
            static_cast<unsigned char>(text[i])), text.size() - i);
        result.emplace_back(text.substr(i, count));
        i += count;
    }
    return result;
}

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> result;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find(separator, start);
        result.push_back(text.substr(start,
            end == std::string_view::npos ? text.size() - start : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

// Returns the first timestamp in the ruby text only when the ruby text
// *starts* with a time tag (no leading text).  Matches the Kirakara Player
// Demo parseKanaTimeAxis: baseTime is set from the first timestamp only when
// there is no text before it; otherwise baseTime stays at the outer context.
std::optional<Seconds> extract_first_ruby_timestamp(std::string_view raw) {
    if (raw.empty() || raw.front() != '[') return std::nullopt;
    const auto close = raw.find(']', 1);
    if (close == std::string_view::npos) return std::nullopt;
    return parse_time_tag(raw.substr(0, close + 1));
}

RubyTrack parse_ruby_track(std::string_view raw, Seconds context_time) {
    RubyTrack track;
    Seconds current_time = context_time;
    std::optional<Seconds> base_time;
    std::size_t position = 0;
    while (position < raw.size()) {
        const auto bracket = raw.find('[', position);
        if (bracket == std::string_view::npos) {
            const auto chunk = raw.substr(position);
            if (!chunk.empty()) {
                track.text.append(chunk);
                if (!base_time) base_time = current_time;
                track.timing.push_back({std::string{chunk},
                    std::max(0.0, current_time - *base_time)});
            }
            break;
        }
        const auto chunk = raw.substr(position, bracket - position);
        if (!chunk.empty()) {
            track.text.append(chunk);
            if (!base_time) base_time = current_time;
            track.timing.push_back({std::string{chunk},
                std::max(0.0, current_time - *base_time)});
        }
        const auto close = raw.find(']', bracket + 1);
        if (close == std::string_view::npos) {
            track.text.append(raw.substr(bracket));
            break;
        }
        if (const auto parsed = parse_time_tag(raw.substr(bracket, close - bracket + 1))) {
            current_time = *parsed;
            if (!base_time) base_time = current_time;
        }
        position = close + 1;
    }
    if (track.timing.size() <= 1) {
        track.timing.clear();
    }
    return track;
}

std::vector<std::string> parse_roles(std::string_view text) {
    std::vector<std::string> roles;
    for (const auto item : split(text, '+')) {
        if (!trim(item).empty()) roles.emplace_back(trim(item));
    }
    return roles;
}

std::vector<Token> lex_line(
    std::string_view line,
    std::vector<std::string>& inherited_roles) {
    std::vector<Token> tokens;
    auto current_roles = inherited_roles;
    bool role_explicit = false;
    Seconds context_time = 0.0;

    for (std::size_t i = 0; i < line.size();) {
        // 反斜杠转义（从左往右）：\\ → 字面 \; \【 → 字面 【（不触发角色标签）。
        // 其他 \x 保持原样，向后兼容既有歌词中的反斜杠。
        if (line[i] == '\\') {
            if (i + 1 < line.size() && line[i + 1] == '\\') {
                LyricChar character;
                character.text = "\\";
                character.roles = current_roles;
                character.role_explicit = role_explicit;
                role_explicit = false;
                tokens.push_back({.character = std::move(character)});
                i += 2;
                continue;
            }
            if (starts_with_at(line, i + 1, kRoleLiteralOpen)) {
                LyricChar character;
                character.text = std::string{kRoleLiteralOpen};
                character.roles = current_roles;
                character.role_explicit = role_explicit;
                role_explicit = false;
                tokens.push_back({.character = std::move(character)});
                i += 1 + kRoleLiteralOpen.size();
                continue;
            }
            LyricChar character;
            character.text = "\\";
            character.roles = current_roles;
            character.role_explicit = role_explicit;
            role_explicit = false;
            tokens.push_back({.character = std::move(character)});
            i += 1;
            continue;
        }
        // 角色标签：仅当按 + 分割后存在非空角色名时才解析（跟随 DOM 解析器）。
        // 非法格式（无任何角色名，如【@】、【@+】）不解析、原样渲染输出。
        if (starts_with_at(line, i, kRoleOpen)) {
            const auto close = line.find(kRoleClose, i + kRoleOpen.size());
            if (close != std::string_view::npos
                    && close > i + kRoleOpen.size()) {
                auto parsed = parse_roles(line.substr(
                    i + kRoleOpen.size(), close - i - kRoleOpen.size()));
                if (!parsed.empty()) {
                    current_roles = std::move(parsed);
                    role_explicit = true;
                    i = close + kRoleClose.size();
                    continue;
                }
            }
        }
        if (line[i] == '[') {
            const auto close = line.find(']', i + 1);
            if (close != std::string_view::npos) {
                if (const auto parsed = parse_time_tag(line.substr(i, close - i + 1))) {
                    context_time = *parsed;
                    tokens.push_back({.is_time = true, .time = *parsed, .character = {}});
                    i = close + 1;
                    continue;
                }
            }
        }
        if (line[i] == '{') {
            const auto close = line.find('}', i + 1);
            const auto divider = line.find('|', i + 1);
            if (close != std::string_view::npos && divider != std::string_view::npos
                    && divider < close) {
                const auto base = line.substr(i + 1, divider - i - 1);
                const auto ruby_raw = line.substr(divider + 1, close - divider - 1);
                const auto ruby_divider = ruby_raw.find('>');
                const auto above = ruby_raw.substr(0, ruby_divider);
                const auto below = ruby_divider == std::string_view::npos
                    ? std::string_view{}
                    : ruby_raw.substr(ruby_divider + 1);

                // Inject a time token from ruby timestamps before the base
                // characters, so that consecutive {…}{…} groups each get their
                // own segment with the correct absolute start time (mirrors
                // the Kirakara Player Demo DOM parser).
                auto ruby_base = extract_first_ruby_timestamp(above);
                if (!ruby_base) ruby_base = extract_first_ruby_timestamp(below);
                if (ruby_base) {
                    context_time = *ruby_base;
                    tokens.push_back({.is_time = true, .time = *ruby_base, .character = {}});
                }

                auto base_chars = inline_ruby_base_characters(base);
                for (std::size_t index = 0; index < base_chars.size(); ++index) {
                    LyricChar character;
                    character.text = std::move(base_chars[index]);
                    character.roles = current_roles;
                    character.role_explicit = role_explicit;
                    role_explicit = false;
                    if (index == 0) {
                        character.ruby_span = base_chars.size();
                        character.ruby_above = parse_ruby_track(above, context_time);
                        character.ruby_below = parse_ruby_track(below, context_time);
                    }
                    tokens.push_back({.character = std::move(character)});
                }
                i = close + 1;
                continue;
            }
        }

        const auto count = std::min(utf8_sequence_length(
            static_cast<unsigned char>(line[i])), line.size() - i);
        LyricChar character;
        character.text = std::string{line.substr(i, count)};
        character.roles = current_roles;
        character.role_explicit = role_explicit;
        role_explicit = false;
        tokens.push_back({.character = std::move(character)});
        i += count;
    }
    if (!current_roles.empty()) inherited_roles = std::move(current_roles);
    return tokens;
}

std::vector<Segment> make_segments(const std::vector<Token>& tokens) {
    std::vector<Segment> segments;
    std::optional<Seconds> segment_start;
    std::vector<LyricChar> chars;
    for (const auto& token : tokens) {
        if (token.is_time) {
            if (!chars.empty()) {
                const auto start = segment_start.value_or(std::max(0.0, token.time - 0.15));
                segments.push_back({start, token.time, std::move(chars)});
                chars.clear();
            }
            segment_start = token.time;
        } else {
            chars.push_back(token.character);
        }
    }
    if (!chars.empty()) {
        segments.push_back({segment_start.value_or(0.0), std::nullopt, std::move(chars)});
    }
    return segments;
}

std::vector<LyricChar> expand_segments(std::vector<Segment> segments) {
    std::vector<LyricChar> result;
    for (auto& segment : segments) {
        const auto end = segment.end.value_or(segment.start + 0.5);
        const auto count = segment.chars.size();
        for (std::size_t i = 0; i < count; ++i) {
            auto character = std::move(segment.chars[i]);
            character.start_time = segment.start + (end - segment.start)
                * static_cast<double>(i) / static_cast<double>(count);
            character.end_time = segment.start + (end - segment.start)
                * static_cast<double>(i + 1) / static_cast<double>(count);
            result.push_back(std::move(character));
        }
    }
    return result;
}

using RubyDictionary = std::unordered_map<std::string, std::vector<RubyDictionaryEntry>>;

bool time_matches(const RubyDictionaryEntry& entry, Seconds time) {
    return std::ranges::any_of(entry.ranges, [time](const auto& range) {
        return time >= range.first && time < range.second;
    });
}

void attach_dictionary_ruby(std::vector<LyricChar>& chars, const RubyDictionary& dictionary) {
    for (std::size_t i = 0; i < chars.size(); ++i) {
        if (chars[i].ruby_span > 0) continue;
        std::string combined = chars[i].text;
        for (std::size_t length = 2; length <= 16 && i + length <= chars.size(); ++length) {
            combined += chars[i + length - 1].text;
            const auto found = dictionary.find(combined);
            if (found == dictionary.end()) continue;
            const auto entry = std::ranges::find_if(found->second,
                [&](const auto& candidate) { return time_matches(candidate, chars[i].start_time); });
            if (entry != found->second.end()) {
                chars[i].ruby_above = entry->ruby;
                chars[i].ruby_span = length;
                break;
            }
        }
    }
    for (auto& character : chars) {
        if (character.ruby_span > 0) continue;
        const auto found = dictionary.find(character.text);
        if (found == dictionary.end()) continue;
        const auto entry = std::ranges::find_if(found->second,
            [&](const auto& candidate) { return time_matches(candidate, character.start_time); });
        if (entry != found->second.end()) {
            character.ruby_above = entry->ruby;
            character.ruby_span = 1;
        }
    }
}

RubyDictionary parse_dictionary(const std::vector<std::string_view>& lines) {
    RubyDictionary dictionary;
    for (auto line : lines) {
        line = trim(line);
        if (line.size() < 7) continue;
        std::string lower_prefix{line.substr(0, std::min<std::size_t>(line.size(), 16))};
        std::ranges::transform(lower_prefix, lower_prefix.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (!lower_prefix.starts_with("@ruby")) continue;
        const auto equals = line.find('=');
        const auto first_comma = line.find(',', equals);
        if (equals == std::string_view::npos || first_comma == std::string_view::npos) continue;
        const std::string base{trim(line.substr(equals + 1, first_comma - equals - 1))};
        const auto parts = split(line.substr(first_comma + 1), ',');
        if (base.empty() || parts.empty()) continue;
        RubyDictionaryEntry entry;
        entry.ruby = parse_ruby_track(trim(parts[0]), 0.0);
        for (std::size_t i = 1; i < parts.size(); i += 2) {
            const auto start = trim(parts[i]).empty()
                ? 0.0 : parse_time_tag(trim(parts[i])).value_or(0.0);
            const auto end = i + 1 >= parts.size() || trim(parts[i + 1]).empty()
                ? std::numeric_limits<Seconds>::infinity()
                : parse_time_tag(trim(parts[i + 1])).value_or(
                    std::numeric_limits<Seconds>::infinity());
            entry.ranges.emplace_back(start, end);
        }
        if (entry.ranges.empty()) {
            entry.ranges.emplace_back(0.0, std::numeric_limits<Seconds>::infinity());
        }
        dictionary[base].push_back(std::move(entry));
    }
    return dictionary;
}

ReferenceLine parse_reference_line(std::string_view line) {
    ReferenceLine result;
    for (std::size_t i = 0; i < line.size();) {
        if (line[i] == '{') {
            const auto divider = line.find('|', i + 1);
            const auto close = line.find('}', i + 1);
            if (divider != std::string_view::npos && close != std::string_view::npos
                    && divider < close) {
                const auto base = line.substr(i + 1, divider - i - 1);
                const auto ruby = line.substr(divider + 1, close - divider - 1);
                const auto split_at = ruby.find('>');
                const auto base_chars = normalized_characters(base);
                ReferenceGroup group;
                group.normalized_start = result.normalized_chars.size();
                group.normalized_span = base_chars.size();
                group.above = parse_ruby_track(ruby.substr(0, split_at), 0.0);
                if (split_at != std::string_view::npos) {
                    group.below = parse_ruby_track(ruby.substr(split_at + 1), 0.0);
                }
                result.normalized_chars.insert(result.normalized_chars.end(),
                    base_chars.begin(), base_chars.end());
                if (group.normalized_span > 0) result.groups.push_back(std::move(group));
                i = close + 1;
                continue;
            }
        }
        const auto count = std::min(utf8_sequence_length(
            static_cast<unsigned char>(line[i])), line.size() - i);
        const auto character = line.substr(i, count);
        if (!is_spacing_character(character)) result.normalized_chars.emplace_back(character);
        i += count;
    }
    return result;
}

std::vector<ReferenceLine> parse_reference_lines(std::string_view utf8) {
    if (utf8.starts_with("\xEF\xBB\xBF")) utf8.remove_prefix(3);
    std::vector<ReferenceLine> result;
    std::size_t start = 0;
    while (start <= utf8.size()) {
        const auto end = utf8.find('\n', start);
        auto line = utf8.substr(start,
            end == std::string_view::npos ? utf8.size() - start : end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        line = trim(line);
        if (!line.empty()) result.push_back(parse_reference_line(line));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

std::vector<std::pair<std::string, std::size_t>> normalized_document_line(
    const LyricLine& line) {
    std::vector<std::pair<std::string, std::size_t>> result;
    for (std::size_t i = 0; i < line.chars.size(); ++i) {
        for (auto character : utf8_characters(line.chars[i].text)) {
            if (!is_spacing_character(character)) result.emplace_back(std::move(character), i);
        }
    }
    return result;
}

bool same_normalized_text(
    const std::vector<std::pair<std::string, std::size_t>>& timed,
    const ReferenceLine& reference) {
    if (timed.size() != reference.normalized_chars.size()) return false;
    for (std::size_t i = 0; i < timed.size(); ++i) {
        if (timed[i].first != reference.normalized_chars[i]) return false;
    }
    return true;
}

} // namespace

LrcParseResult parse_lrc(std::string_view utf8, const EngineConfig& config) {
    LrcParseResult output;
    if (utf8.starts_with("\xEF\xBB\xBF")) utf8.remove_prefix(3);

    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= utf8.size()) {
        const auto end = utf8.find('\n', start);
        auto line = utf8.substr(start,
            end == std::string_view::npos ? utf8.size() - start : end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!trim(line).empty()) lines.push_back(line);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }

    const auto dictionary = parse_dictionary(lines);
    std::vector<LyricLine> parsed_lines;
    std::vector<bool> automatic_tail;
    std::vector<std::string> inherited_roles;
    for (auto source_line : lines) {
        const auto clean = trim(source_line);
        std::string prefix{clean.substr(0, std::min<std::size_t>(clean.size(), 8))};
        std::ranges::transform(prefix, prefix.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (prefix.starts_with("@ruby")) continue;

        auto tokens = lex_line(clean, inherited_roles);
        if (std::ranges::none_of(tokens, [](const auto& token) { return token.is_time; })) {
            continue;
        }
        const bool ends_with_time = !tokens.empty() && tokens.back().is_time;
        auto chars = expand_segments(make_segments(tokens));
        if (chars.empty()) continue;
        attach_dictionary_ruby(chars, dictionary);
        LyricLine line;
        line.chars = std::move(chars);
        line.start_time = line.chars.front().start_time;
        line.end_time = line.chars.back().end_time;
        parsed_lines.push_back(std::move(line));
        automatic_tail.push_back(!ends_with_time);
    }

    for (std::size_t i = 0; i + 1 < parsed_lines.size(); ++i) {
        if (!automatic_tail[i]) continue;
        const auto gap = parsed_lines[i + 1].start_time - parsed_lines[i].end_time;
        if (gap > 0.0 && gap < 5.0) {
            parsed_lines[i].chars.back().end_time += gap;
            parsed_lines[i].end_time += gap;
        }
    }
    if (parsed_lines.empty() && !trim(utf8).empty()) {
        output.warnings.emplace_back("No timed lyric lines were recognized.");
    }
    output.document = prepare_timeline(parsed_lines, config);
    return output;
}

ReferenceMergeResult apply_lyric_reference(
    PreparedDocument& document,
    std::string_view utf8_reference) {
    ReferenceMergeResult result;
    const auto references = parse_reference_lines(utf8_reference);
    std::size_t timed_cursor = 0;
    for (const auto& reference : references) {
        bool matched = false;
        for (std::size_t line_index = timed_cursor;
                line_index < document.lines.size(); ++line_index) {
            auto normalized = normalized_document_line(document.lines[line_index]);
            if (!same_normalized_text(normalized, reference)) continue;
            matched = true;
            timed_cursor = line_index + 1;
            ++result.matched_lines;
            auto& line = document.lines[line_index];
            for (const auto& group : reference.groups) {
                if (group.normalized_start >= normalized.size()
                        || group.normalized_span == 0
                        || group.normalized_start + group.normalized_span > normalized.size()) {
                    continue;
                }
                const auto first_char = normalized[group.normalized_start].second;
                const auto last_char = normalized[
                    group.normalized_start + group.normalized_span - 1].second;
                if (first_char >= line.chars.size() || last_char < first_char) continue;
                line.chars[first_char].ruby_span = last_char - first_char + 1;
                line.chars[first_char].ruby_above = group.above;
                line.chars[first_char].ruby_below = group.below;
                ++result.attached_groups;
            }
            break;
        }
        if (!matched && !reference.normalized_chars.empty()) {
            result.warnings.emplace_back("Reference line did not match a timed line.");
        }
    }
    if (references.empty() && !trim(utf8_reference).empty()) {
        result.warnings.emplace_back("No reference lyric lines were recognized.");
    }
    return result;
}

} // namespace kirakara::show
