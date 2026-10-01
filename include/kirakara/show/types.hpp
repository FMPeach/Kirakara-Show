#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace kirakara::show {

using Seconds = double;

struct Color {
    float r{1.0F};
    float g{1.0F};
    float b{1.0F};
    float a{1.0F};
};

struct PaintPair {
    Color fill_before{};
    Color fill_after{0.0F, 0.0F, 0.647F, 1.0F};
    Color stroke_before{0.0F, 0.0F, 0.0F, 1.0F};
    Color stroke_after{};
    float stroke_width{4.0F};
};

struct TimedText {
    std::string text;
    Seconds offset{};
};

struct RubyTrack {
    std::string text;
    std::vector<TimedText> timing;
};

struct LyricChar {
    std::string text;
    Seconds start_time{};
    Seconds end_time{};
    RubyTrack ruby_above;
    RubyTrack ruby_below;
    std::size_t ruby_span{};
    std::vector<std::string> roles;
    bool role_explicit{};
};

struct LyricLine {
    std::vector<LyricChar> chars;
    Seconds start_time{};
    Seconds end_time{};

    // Filled by prepare_timeline. Callers may also provide already prepared data.
    std::size_t paragraph{};
    std::size_t line_in_paragraph{};
    Seconds paragraph_start_time{};
    Seconds entry_time{};
    Seconds walk_done_time{};
    bool first_in_paragraph{};
    bool last_in_paragraph{};
};

struct IndicatorStyle {
    bool enabled{true};
    Seconds duration{3.0};
    float fade_ratio{};
    float size{34.0F};
    float spacing{12.0F};
    float stroke_width{3.0F};
    float offset_x{};
    float offset_y{8.0F};
    Color fill{};
    Color stroke{0.0F, 0.0F, 0.0F, 1.0F};
};

enum class FadeMode : std::uint8_t {
    disabled,
    paragraph_edges,
    every_line,
};

struct EngineConfig {
    Seconds normal_entry_buffer{2.0};
    Seconds exit_buffer{2.0};
    Seconds fade_duration{0.666};
    FadeMode fade_mode{FadeMode::paragraph_edges};
    IndicatorStyle indicator{};

    [[nodiscard]] Seconds entry_buffer() const noexcept {
        return indicator.enabled
            ? fade_duration + 0.5 + indicator.duration
            : normal_entry_buffer;
    }
};

struct PreparedDocument {
    std::vector<LyricLine> lines;
};

struct CharacterState {
    std::size_t char_index{};
    double progress{}; // temporal progress in [0, 1]
};

struct LineState {
    std::size_t source_line{};
    double opacity{1.0};
    std::vector<CharacterState> chars;
    // Left-to-right order, matching the DOM renderer's reversed dot array.
    std::array<double, 4> indicator_opacity{};
    bool indicator_visible{};
};

struct FrameState {
    Seconds time{};
    std::array<bool, 2> has_line{};
    std::array<LineState, 2> slots{};
};

} // namespace kirakara::show
