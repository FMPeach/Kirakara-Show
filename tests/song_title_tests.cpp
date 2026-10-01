#include "kirakara/show/config.hpp"
#include "kirakara/show/project_parser.hpp"
#include "kirakara/show/song_title.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

using namespace kirakara::show;

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void expect_near(double actual, double expected, std::string_view message) {
    expect(std::abs(actual - expected) < 1.0e-6, message);
}

void parser_flattens_editor_groups_into_render_blocks() {
    auto config = default_app_config();
    expect(load_app_config_json(R"({
        "fontSize": 62,
        "songTitle": {
            "enabled": true,
            "durationSec": 5,
            "textFade": true,
            "prelude": {
                "enabled": true,
                "fadeEnabled": true,
                "fadeDurationMs": 666,
                "backgroundImage": {
                    "name": "opening.png",
                    "mime": "image/png",
                    "base64": "iVBORw0KGgo="
                }
            },
            "groups": [
                {
                    "kind": "not-an-engine-type",
                    "name": "editor-only label",
                    "id": "editor-block-1",
                    "value": "\u5f71\u8272\u821e",
                    "style": {
                        "fontFamily": "'Noto Sans JP', sans-serif",
                        "fontSize": 76,
                        "fontBold": false,
                        "letterSpacing": 7,
                        "x": 640,
                        "y": 231,
                        "align": "center",
                        "color": "#3e008a",
                        "strokeColor": "#ffffff",
                        "strokeWidth": 2
                    }
                },
                {
                    "kind": "anything",
                    "name": "another editor label",
                    "rows": [
                        {"before":"artist","separator":":","after":"MyGO!!!!!"}
                    ],
                    "style": {"fontSize":36,"align":"center-left"}
                }
            ]
        }
    })", config), "nested title JSON parse");

    expect(config.style.font_size == 62.0F,
        "nested title font size does not replace lyric font size");
    expect(config.song_title.enabled, "title enabled");
    expect_near(config.song_title.duration, 5.0, "title duration");
    expect(config.song_title.blocks.size() == 2, "render blocks parsed");
    expect(config.song_title.blocks[0].lines.size() == 1
            && config.song_title.blocks[0].lines.front()
                == "\xE5\xBD\xB1\xE8\x89\xB2\xE8\x88\x9E",
        "unicode value normalized to a render line");
    expect(config.song_title.blocks[0].style.font_size == 76.0F,
        "block font size");
    expect(config.song_title.blocks[0].style.font_families.size() == 2,
        "block CSS font family chain");
    expect(config.song_title.blocks[1].lines.size() == 1
            && config.song_title.blocks[1].lines.front()
                == "artist:MyGO!!!!!",
        "editor row structure flattened before entering the engine");
}

void timeline_matches_player_demo() {
    auto config = default_song_title_config();
    config.enabled = true;
    config.duration = 5.0;

    auto timeline = song_title_timeline(config);
    expect_near(timeline.title_start, 0.0, "normal title start");
    expect_near(timeline.title_end, 5.0, "normal title end");
    expect_near(song_title_text_opacity(0.333, config, 0.666), 0.5,
        "normal title fade in");
    expect_near(song_title_text_opacity(4.667, config, 0.666), 0.5,
        "normal title fade out");

    config.text_fade = false;
    expect_near(song_title_text_opacity(0.0, config, 0.666), 1.0,
        "disabled text fade is opaque at media zero");
}

void empty_primary_title_is_valid() {
    auto config = default_song_title_config();
    config.enabled = true;
    SongTitleBlock block;
    block.lines = {"", "artist:Kirakara"};
    config.blocks.push_back(block);
    expect(song_title_has_drawable_content(config),
        "an empty line does not hide another drawable line");

    config.blocks.front().lines = {""};
    expect(!song_title_has_drawable_content(config),
        "entirely empty title is a valid drawing no-op");
    expect(song_title_visible(0.0, config),
        "empty title preserves its configured media interval");

    const auto parsed = parse_project_text(R"KRL(config {
        "songTitle": {
            "enabled": true,
            "groups": [{"kind": "title", "value": ""}]
        }
    }
    [00:01:00]A[00:02:00]
    )KRL");
    expect(parsed.has_config && parsed.config_valid,
        "enabled empty title KRL is valid");
    expect(parsed.config.song_title.enabled,
        "empty title does not disable the title engine");
    expect(parsed.config.song_title.blocks.empty(),
        "empty editor groups do not enter the renderer model");
    expect(!song_title_has_drawable_content(parsed.config.song_title),
        "enabled empty KRL title remains a drawing no-op");
}

void alignment_matches_player_demo() {
    expect_near(song_title_line_start_x(SongTitleAlign::left,
        640.0F, 200.0F, 300.0F), 640.0, "left alignment");
    expect_near(song_title_line_start_x(SongTitleAlign::center_left,
        640.0F, 200.0F, 300.0F), 490.0, "center-left paragraph edge");
    expect_near(song_title_line_start_x(SongTitleAlign::center,
        640.0F, 200.0F, 300.0F), 540.0, "center alignment");
    expect_near(song_title_line_start_x(SongTitleAlign::center_right,
        640.0F, 200.0F, 300.0F), 590.0, "center-right paragraph edge");
    expect_near(song_title_line_start_x(SongTitleAlign::right,
        640.0F, 200.0F, 300.0F), 440.0, "right alignment");
}

void project_parser_handles_json_braces_and_role_blocks() {
    const auto parsed = parse_project_text(R"KRL(config {
        "songTitle": {
            "enabled": true,
            "durationSec": 5,
            "groups": [{
                "kind": "title",
                "value": "title } { text",
                "style": {"fontSize": 76}
            }]
        }
    }
    role { "name": "ignored } metadata" }
    [00:01:00]A[00:02:00]
    )KRL");
    expect(parsed.has_config && parsed.config_valid,
        "KRL config block parsed");
    expect(parsed.config.song_title.blocks.front().lines.front()
        == "title } { text", "braces inside JSON string preserved");
    expect(parsed.document.lines.size() == 1,
        "role metadata stripped before lyric parse");
    expect(parsed.document.lines.front().chars.front().text == "A",
        "lyric content retained");

    const auto plain = parse_project_text(
        "[00:01:00]literal config { text[00:02:00]\n");
    expect(!plain.has_config,
        "inline lyric text is not mistaken for a config block");
}

} // namespace

int main() {
    parser_flattens_editor_groups_into_render_blocks();
    timeline_matches_player_demo();
    empty_primary_title_is_valid();
    alignment_matches_player_demo();
    project_parser_handles_json_braces_and_role_blocks();
    std::cout << "All song title tests passed.\n";
}
