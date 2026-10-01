#include "kirakara/show/timeline.hpp"
#include "kirakara/show/lrc_parser.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <string_view>
#include <vector>

using namespace kirakara::show;

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void expect_near(double actual, double expected, std::string_view message) {
    expect(std::abs(actual - expected) < 1.0e-9, message);
}

LyricLine line(double start, double end, std::string text = "字") {
    LyricLine result;
    result.chars.push_back({std::move(text), start, end});
    result.start_time = start;
    result.end_time = end;
    return result;
}

void temporal_progress_matches_dom() {
    expect_near(temporal_progress(0.0, 1.0, 2.0), 0.0, "before char");
    expect_near(temporal_progress(1.5, 1.0, 2.0), 0.5, "inside char");
    expect_near(temporal_progress(2.0, 1.0, 2.0), 1.0, "after char");
}

void first_pair_enters_together() {
    EngineConfig config;
    config.indicator.enabled = false;
    const std::vector<LyricLine> input{line(10.0, 11.0), line(12.0, 13.0)};
    const auto doc = prepare_timeline(input, config);
    expect_near(doc.lines[0].entry_time, 8.0, "first entry");
    expect_near(doc.lines[1].entry_time, 8.0, "paired entry");
    expect(doc.lines[0].first_in_paragraph, "first slot is paragraph edge");
    expect(doc.lines[1].first_in_paragraph, "second slot is paragraph edge");
}

void slot_replacement_matches_dom() {
    EngineConfig config;
    config.indicator.enabled = false;
    const std::vector<LyricLine> input{
        line(1.0, 2.0, "A"), line(1.2, 2.2, "B"), line(2.1, 3.0, "C")};
    const auto doc = prepare_timeline(input, config);
    const auto early = evaluate_frame(doc, 1.5, config);
    expect(early.has_line[0] && early.slots[0].source_line == 0, "old slot line");
    const auto later = evaluate_frame(doc, 2.15, config);
    expect(later.has_line[0] && later.slots[0].source_line == 2, "new slot line");
}

void grouped_timing_drives_base_characters() {
    std::vector<LyricChar> chars{
        {.text = "今", .start_time = 0.0, .end_time = 1.0},
        {.text = "日", .start_time = 1.0, .end_time = 2.0},
    };
    const std::vector<TimedText> ruby{{"きょ", 0.0}, {"う", 1.0}};
    expect_near(grouped_character_progress(chars, ruby, 0, 0.5), 0.5,
        "first base char follows first ruby segment");
    expect_near(grouped_character_progress(chars, ruby, 1, 0.5), 0.0,
        "second base char not started");
    expect_near(grouped_character_progress(chars, ruby, 1, 1.5), 0.5,
        "second base char follows second ruby segment");
}

void fades_and_indicator_match_dom_formulas() {
    EngineConfig config;
    config.indicator.duration = 4.0;
    config.indicator.fade_ratio = 1.0F;
    LyricLine value = line(10.0, 12.0);
    value.entry_time = 5.0;
    value.first_in_paragraph = true;
    value.last_in_paragraph = true;
    value.line_in_paragraph = 0;
    expect_near(line_opacity(value, 5.0 + config.fade_duration / 2.0, config), 0.5, "fade in");
    const auto dots = indicator_opacities(value, 6.5, config);
    expect_near(dots[3], 0.5, "DOM rightmost first dot fading");
    expect_near(dots[0], 1.0, "DOM leftmost last dot untouched");
}

void parser_reads_demo_syntax() {
    const auto parsed = parse_lrc(
        "@ruby=今日,きょ[00:01:00]う\n"
        "【@A+B】[00:10:00]{今日|きょ[00:11:00]う>today}[00:12:00]！[00:13:00]\n");
    expect(parsed.document.lines.size() == 1, "one parsed line");
    const auto& chars = parsed.document.lines.front().chars;
    expect(chars.size() == 3, "UTF-8 base characters expanded");
    expect(chars[0].ruby_span == 2, "inline ruby span");
    expect(chars[0].ruby_above.text == "きょう", "upper ruby text");
    expect(chars[0].ruby_below.text == "today", "lower ruby text");
    expect(chars[0].roles.size() == 2 && chars[1].roles.size() == 2,
        "combined role inherited");
    expect_near(chars[0].start_time, 10.0, "first base start");
    expect_near(chars[1].end_time, 12.0, "second base end");
}

void parser_ignores_inline_ruby_base_time_tags() {
    const auto parsed = parse_lrc(
        "[02:46:03]{\xE8\xB2\xB4[02:46:68]\xE6\x96\xB9|"
        "[02:46:03]\xE3\x81\x82[02:46:36]\xE3\x81\xAA[02:46:68]\xE3\x81\x9F>"
        "[02:46:03]a[02:46:36]na[02:46:68]ta}[02:47:29]\n");
    expect(parsed.document.lines.size() == 1, "inline ruby base tag line parsed");
    const auto& chars = parsed.document.lines.front().chars;
    expect(chars.size() == 2, "base time tags are not lyric characters");
    expect(chars[0].text == "\xE8\xB2\xB4", "first base character preserved");
    expect(chars[1].text == "\xE6\x96\xB9", "second base character preserved");
    expect(chars[0].ruby_span == 2, "ruby span ignores base time tags");
    expect(chars[0].ruby_above.text
            == "\xE3\x81\x82\xE3\x81\xAA\xE3\x81\x9F", "upper ruby text stripped");
    expect(chars[0].ruby_below.text == "anata", "lower ruby text stripped");
    expect_near(chars[0].start_time, 166.03, "group start follows ruby timestamp");
    expect_near(chars[0].end_time, 166.66, "base tag does not split main timing");
    expect_near(chars[1].start_time, 166.66, "second base char is evenly segmented");
    expect_near(chars[1].end_time, 167.29, "group end follows outer tag");
    expect(chars[0].ruby_above.timing.size() == 3, "upper ruby timing preserved");
    expect_near(chars[0].ruby_above.timing[2].offset, 0.65,
        "upper ruby absolute tag becomes offset");
}

void parser_reads_real_world_character_timestamps() {
    EngineConfig config;
    config.indicator.enabled = false;
    const auto parsed = parse_lrc(
        "[00:00:17]あ[00:00:27]あ[00:00:38]待[00:00:49]\n", config);
    expect(parsed.document.lines.size() == 1, "real line parsed");
    const auto& chars = parsed.document.lines.front().chars;
    expect(chars.size() == 3, "real line char count");
    expect_near(chars[0].start_time, 0.17, "centisecond timestamp");
    expect_near(chars[2].end_time, 0.49, "explicit final timestamp");
}

void untimed_reference_transfers_segmentation_and_ruby() {
    EngineConfig config;
    config.indicator.enabled = false;
    auto parsed = parse_lrc(
        "[00:01:00]た[00:02:00]だ[00:03:00]　[00:04:00]静[00:05:00]か[00:06:00]に[00:07:00]\n",
        config);
    const auto merged = apply_lyric_reference(parsed.document,
        "ただ　{静|しず}かに\n");
    expect(merged.matched_lines == 1, "reference line matched ignoring spaces");
    expect(merged.attached_groups == 1, "reference ruby group attached");
    const auto& chars = parsed.document.lines.front().chars;
    expect(chars[3].text == "静", "reference mapped to original timed index");
    expect(chars[3].ruby_span == 1, "reference span preserved");
    expect(chars[3].ruby_above.text == "しず", "reference reading transferred");
    expect_near(chars[3].start_time, 4.0, "reference never changes timing");
}

void reference_segment_and_midline_roles_match_dom_parser() {
    EngineConfig config;
    config.indicator.enabled = false;
    const auto parsed = parse_lrc(
        "【@上+菜】[01:31:40]ル[01:31:54]ビィ[01:31:77]ちゃん！[01:32:39]　"
        "【@黑】[01:32:55]はい！[01:33:42]\n", config);
    expect(parsed.document.lines.size() == 1, "reference segmented line parsed");
    const auto& chars = parsed.document.lines.front().chars;
    expect(chars[0].roles.size() == 2 && chars[0].role_explicit,
        "combined role starts line");
    expect(chars[1].text == "ビ" && chars[2].text == "ィ",
        "multi-character segment splits to Unicode characters");
    expect_near(chars[1].start_time, 91.54, "segment first half start");
    expect_near(chars[1].end_time, 91.655, "segment first half end");
    expect_near(chars[2].end_time, 91.77, "segment second half end");
    const auto black = std::ranges::find_if(chars, [](const auto& character) {
        return character.role_explicit && character.roles.size() == 1
            && character.roles.front() == "黑";
    });
    expect(black != chars.end() && black->text == "は",
        "mid-line role switch attaches to first following character");
}

void ascii_brackets_never_define_roles() {
    EngineConfig config;
    config.indicator.enabled = false;
    const auto parsed = parse_lrc(
        "[A+B][00:01:00]歌[00:02:00]\n"
        "【A+B】[00:03:00]声[00:04:00]\n"
        "【@A+B】[00:05:00]和[00:06:00]\n", config);
    expect(parsed.document.lines.size() == 3,
        "bracket role guard parsed three lines");

    const auto& bracket_chars = parsed.document.lines[0].chars;
    expect(!bracket_chars.empty(), "ascii bracket line has characters");
    expect(bracket_chars.front().roles.empty(),
        "ascii [A+B] is literal text, not a role marker");
    bool has_literal_bracket = false;
    for (const auto& character : bracket_chars) {
        if (character.text == "[" || character.text == "A"
                || character.text == "+") {
            has_literal_bracket = true;
            break;
        }
    }
    expect(has_literal_bracket, "ascii bracket marker remains lyric text");

    const auto& role_chars = parsed.document.lines[1].chars;
    expect(!role_chars.empty(), "fullwidth plain bracket line has characters");
    expect(role_chars.front().roles.empty(),
        "plain fullwidth 【A+B】 is literal text, not a role marker");

    const auto& at_role_chars = parsed.document.lines[2].chars;
    expect(!at_role_chars.empty(), "fullwidth @ bracket line has characters");
    expect(at_role_chars.front().roles.size() == 2
            && at_role_chars.front().roles[0] == "A"
            && at_role_chars.front().roles[1] == "B",
        "fullwidth 【@A+B】 defines two roles");
}

void role_tag_edge_cases_match_dom_parser() {
    EngineConfig config;
    config.indicator.enabled = false;
    // 极端：【@@A+@B】 → @A 与 @B 是独立角色（@ 属于角色名，不触发嵌套）
    const auto extreme = parse_lrc(
        "【@@A+@B】[00:01:00]今[00:02:00]\n", config);
    expect(extreme.document.lines.size() == 1, "extreme role line parsed");
    const auto& extreme_chars = extreme.document.lines.front().chars;
    expect(extreme_chars.front().roles.size() == 2
            && extreme_chars.front().roles[0] == "@A"
            && extreme_chars.front().roles[1] == "@B",
        "【@@A+@B】 yields independent @A/@B roles");

    // 转义：\【@111】 → 字面文本【@111】，不触发角色
    const auto escaped = parse_lrc(
        "\\\xE3\x80\x90@111\xE3\x80\x91[00:03:00]\xE6\xAD\x8C[00:04:00]\n",
        config);
    expect(escaped.document.lines.size() == 1, "escaped role line parsed");
    const auto& escaped_chars = escaped.document.lines.front().chars;
    expect(escaped_chars.front().text == "\xE3\x80\x90",
        "escaped bracket is literal 【");
    expect(escaped_chars.front().roles.empty(),
        "escaped 【@111】 never defines roles");
    std::string escaped_plain;
    for (const auto& character : escaped_chars) escaped_plain += character.text;
    expect(escaped_plain == "\xE3\x80\x90@111\xE3\x80\x91\xE6\xAD\x8C",
        "escaped marker stays as literal text 【@111】歌");

    // 空角色标签：【@】 是非法格式 → 不解析，原样渲染输出（跟随 DOM 解析器）
    const auto bare_role = parse_lrc(
        "【@】[00:05:00]\xE5\xA3\xB0[00:06:00]\n", config);
    expect(bare_role.document.lines.size() == 1, "bare role line parsed");
    const auto& bare_chars = bare_role.document.lines.front().chars;
    expect(!bare_chars.empty() && bare_chars.front().roles.empty(),
        "bare 【@】 is literal, never defines roles");
    std::string bare_plain;
    for (const auto& character : bare_chars) bare_plain += character.text;
    expect(bare_plain == "\xE3\x80\x90@\xE3\x80\x91\xE5\xA3\xB0",
        "bare 【@】 renders as literal text 【@】声");

    // 有 + 但无角色名：【@+】 是非法格式 → 不解析，原样渲染输出（跟随 DOM）
    const auto plus_role = parse_lrc(
        "【@+】[00:07:00]\xE5\x92\x8C[00:08:00]\n", config);
    expect(plus_role.document.lines.size() == 1, "plus role line parsed");
    const auto& plus_chars = plus_role.document.lines[0].chars;
    expect(!plus_chars.empty() && plus_chars.front().roles.empty(),
        "bare 【@+】 is literal, never defines roles");
    std::string plus_plain;
    for (const auto& character : plus_chars) plus_plain += character.text;
    expect(plus_plain == "\xE3\x80\x90@+\xE3\x80\x91\xE5\x92\x8C",
        "bare 【@+】 renders as literal text 【@+】和");

    // 一端有角色名仍合法：【@A+】 → 角色 A；【@+B】 → 角色 B
    const auto edge_left = parse_lrc(
        "【@A+】[00:09:00]\xE7\xA9\xBA[00:10:00]\n", config);
    const auto& edge_left_chars = edge_left.document.lines[0].chars;
    expect(edge_left_chars.front().roles.size() == 1
            && edge_left_chars.front().roles[0] == "A",
        "trailing + 【@A+】 keeps role A");
    const auto edge_right = parse_lrc(
        "【@+B】[00:11:00]\xE9\x96\x93[00:12:00]\n", config);
    const auto& edge_right_chars = edge_right.document.lines[0].chars;
    expect(edge_right_chars.front().roles.size() == 1
            && edge_right_chars.front().roles[0] == "B",
        "leading + 【@+B】 keeps role B");
}

} // namespace

int main() {
    temporal_progress_matches_dom();
    first_pair_enters_together();
    slot_replacement_matches_dom();
    grouped_timing_drives_base_characters();
    fades_and_indicator_match_dom_formulas();
    parser_reads_demo_syntax();
    parser_ignores_inline_ruby_base_time_tags();
    parser_reads_real_world_character_timestamps();
    untimed_reference_transfers_segmentation_and_ruby();
    reference_segment_and_midline_roles_match_dom_parser();
    ascii_brackets_never_define_roles();
    role_tag_edge_cases_match_dom_parser();
    std::cout << "All Kirakara Show timeline tests passed.\n";
}
