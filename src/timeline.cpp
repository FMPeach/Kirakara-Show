#include "kirakara/show/timeline.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace kirakara::show {
namespace {

constexpr Seconds kWalkProtect = 1.0;
constexpr Seconds kProtectMinMargin = 2.5;

double clamp01(double value) noexcept {
    return std::clamp(value, 0.0, 1.0);
}

void infer_line_bounds(LyricLine& line) {
    if (line.chars.empty()) {
        return;
    }
    line.start_time = line.chars.front().start_time;
    line.end_time = line.chars.back().end_time;
}

std::span<const TimedText> group_timing(const LyricChar& first) {
    if (first.ruby_above.timing.size() > 1) {
        return first.ruby_above.timing;
    }
    if (first.ruby_below.timing.size() > 1) {
        return first.ruby_below.timing;
    }
    return {};
}

} // namespace

PreparedDocument prepare_timeline(
    std::span<const LyricLine> source,
    const EngineConfig& config) {
    PreparedDocument result;
    result.lines.assign(source.begin(), source.end());
    if (result.lines.empty()) {
        return result;
    }

    for (auto& line : result.lines) {
        infer_line_bounds(line);
    }

    const auto entry_buffer = config.entry_buffer();
    std::size_t paragraph = 0;
    std::size_t line_in_paragraph = 0;
    Seconds paragraph_start = result.lines.front().start_time;

    for (std::size_t i = 0; i < result.lines.size(); ++i) {
        auto& line = result.lines[i];
        if (i > 0 && line.start_time - result.lines[i - 1].end_time
                > entry_buffer + config.exit_buffer) {
            ++paragraph;
            line_in_paragraph = 0;
            paragraph_start = line.start_time;
        }
        line.paragraph = paragraph;
        line.line_in_paragraph = line_in_paragraph++;
        line.paragraph_start_time = paragraph_start;
        line.entry_time = line.start_time - entry_buffer;
        line.walk_done_time = line.end_time;
    }

    // The first two lines of a paragraph enter together in the DOM preview.
    for (auto& line : result.lines) {
        if (line.line_in_paragraph == 1) {
            line.entry_time = line.paragraph_start_time - entry_buffer;
        }
    }

    // Lines i and i+2 share a visual slot.
    for (std::size_t i = 0; i + 2 < result.lines.size(); ++i) {
        auto& current = result.lines[i];
        auto& next_same_slot = result.lines[i + 2];
        if (next_same_slot.paragraph != current.paragraph) {
            continue;
        }
        if (next_same_slot.entry_time > current.end_time + config.exit_buffer) {
            current.walk_done_time = current.end_time + config.exit_buffer;
            next_same_slot.entry_time = current.walk_done_time;
        }
    }

    if (config.indicator.enabled) {
        for (std::size_t i = 0; i + 2 < result.lines.size(); ++i) {
            auto& current = result.lines[i];
            const auto& next_same_slot = result.lines[i + 2];
            if (next_same_slot.paragraph != current.paragraph) {
                continue;
            }
            const auto proposed = current.walk_done_time + kWalkProtect;
            if (next_same_slot.start_time >= proposed + kProtectMinMargin) {
                current.walk_done_time = proposed;
            }
        }
    }

    for (std::size_t i = 0; i < result.lines.size(); ++i) {
        auto& line = result.lines[i];
        line.first_in_paragraph = line.line_in_paragraph <= 1;
        line.last_in_paragraph = i + 1 == result.lines.size()
            || result.lines[i + 1].paragraph != line.paragraph;
    }
    return result;
}

double temporal_progress(
    Seconds time,
    Seconds start_time,
    Seconds end_time) noexcept {
    if (time < start_time) {
        return 0.0;
    }
    if (time >= end_time) {
        return 1.0;
    }
    const auto duration = end_time - start_time;
    if (!(duration > 0.0) || !std::isfinite(duration)) {
        return 1.0;
    }
    return clamp01((time - start_time) / duration);
}

double grouped_character_progress(
    std::span<const LyricChar> group_chars,
    std::span<const TimedText> ruby_timing,
    std::size_t character_index,
    Seconds time) noexcept {
    const auto character_count = group_chars.size();
    const auto syllable_count = ruby_timing.size();
    if (character_count == 0 || syllable_count == 0
            || character_index >= character_count) {
        return 0.0;
    }

    const auto group_start = group_chars.front().start_time;
    const auto group_end = group_chars.back().end_time;
    const auto group_span = group_end - group_start;

    std::size_t segment = 0;
    for (std::size_t i = 0; i < syllable_count; ++i) {
        const auto segment_start = group_start + ruby_timing[i].offset;
        const auto segment_end = i + 1 < syllable_count
            ? group_start + (ruby_timing[i + 1].offset != 0.0
                ? ruby_timing[i + 1].offset
                : group_span * static_cast<double>(i + 1) / static_cast<double>(syllable_count))
            : group_end;
        if (time >= segment_start && time < segment_end) {
            segment = i;
            break;
        }
        if (i + 1 == syllable_count && time >= segment_end) {
            segment = i;
        }
    }

    const auto segment_start = group_start + ruby_timing[segment].offset;
    const auto segment_end = segment + 1 < syllable_count
        ? group_start + (ruby_timing[segment + 1].offset != 0.0
            ? ruby_timing[segment + 1].offset
            : group_span * static_cast<double>(segment + 1) / static_cast<double>(syllable_count))
        : group_end;

    double segment_progress = 0.0;
    if (time >= group_end) {
        segment_progress = 1.0;
    } else if (time >= group_start) {
        segment_progress = temporal_progress(time, segment_start, segment_end);
    }

    const auto global = (static_cast<double>(segment) + segment_progress)
        / static_cast<double>(syllable_count)
        * static_cast<double>(character_count);
    return clamp01(global - static_cast<double>(character_index));
}

double line_opacity(
    const LyricLine& line,
    Seconds time,
    const EngineConfig& config) noexcept {
    if (config.fade_mode == FadeMode::disabled) {
        return 1.0;
    }

    const auto duration = config.fade_duration > 0.0
        ? config.fade_duration
        : Seconds{0.666};
    const bool fade_in = config.fade_mode == FadeMode::every_line
        || line.first_in_paragraph;
    const bool fade_out = config.fade_mode == FadeMode::every_line
        || line.last_in_paragraph;
    double opacity = 1.0;
    if (fade_in && time < line.entry_time + duration) {
        opacity = std::max(0.0, (time - line.entry_time) / duration);
    }
    const auto exit_time = line.end_time + config.exit_buffer;
    if (fade_out && time > exit_time - duration) {
        opacity = std::min(opacity, std::max(0.0, (exit_time - time) / duration));
    }
    return clamp01(opacity);
}

std::array<double, 4> indicator_opacities(
    const LyricLine& line,
    Seconds time,
    const EngineConfig& config) noexcept {
    std::array<double, 4> logical{1.0, 1.0, 1.0, 1.0};
    if (!config.indicator.enabled || !line.first_in_paragraph
            || line.line_in_paragraph != 0) {
        return {0.0, 0.0, 0.0, 0.0};
    }

    const auto duration = config.indicator.duration > 0.0
        ? config.indicator.duration
        : Seconds{4.0};
    const auto quarter = duration / 4.0;
    const auto fade_ratio = clamp01(config.indicator.fade_ratio);
    for (std::size_t dot = 0; dot < logical.size(); ++dot) {
        const auto disappear_at = line.start_time - duration
            + static_cast<double>(dot + 1) * quarter;
        const auto fade_start = disappear_at - quarter;
        const auto fade_duration = quarter * fade_ratio;
        const auto fade_end = fade_start + fade_duration;
        if (time >= fade_end) {
            logical[dot] = 0.0;
        } else if (time > fade_start && fade_duration > 0.0) {
            logical[dot] = clamp01((fade_end - time) / fade_duration);
        }
    }
    return {logical[3], logical[2], logical[1], logical[0]};
}

FrameState evaluate_frame(
    const PreparedDocument& document,
    Seconds time,
    const EngineConfig& config) {
    FrameState frame;
    frame.time = time;

    std::array<std::size_t, 2> selected{
        std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max()};
    std::array<std::size_t, 2> selected_paragraph{
        std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max()};
    std::array<bool, 2> walking{};

    for (std::size_t i = 0; i < document.lines.size(); ++i) {
        const auto& line = document.lines[i];
        const bool in_window = time >= line.entry_time
            && time <= line.end_time + config.exit_buffer;
        if (!in_window) {
            continue;
        }
        const auto slot = line.line_in_paragraph % 2;
        if (line.paragraph != selected_paragraph[slot]) {
            selected[slot] = i;
            selected_paragraph[slot] = line.paragraph;
            walking[slot] = time < line.walk_done_time;
        } else if (!walking[slot]) {
            selected[slot] = i;
            walking[slot] = time < line.walk_done_time;
        }
    }

    for (std::size_t slot = 0; slot < selected.size(); ++slot) {
        if (selected[slot] == std::numeric_limits<std::size_t>::max()) {
            continue;
        }
        frame.has_line[slot] = true;
        auto& state = frame.slots[slot];
        state.source_line = selected[slot];
        const auto& line = document.lines[selected[slot]];
        state.opacity = line_opacity(line, time, config);
        state.indicator_visible = config.indicator.enabled
            && line.first_in_paragraph && line.line_in_paragraph == 0;
        state.indicator_opacity = indicator_opacities(line, time, config);
        state.chars.reserve(line.chars.size());

        for (std::size_t i = 0; i < line.chars.size();) {
            const auto& character = line.chars[i];
            const auto span = std::max<std::size_t>(1, character.ruby_span);
            const auto safe_span = std::min(span, line.chars.size() - i);
            const auto timing = group_timing(character);
            if (!timing.empty()) {
                const auto group = std::span<const LyricChar>{line.chars}.subspan(i, safe_span);
                for (std::size_t j = 0; j < safe_span; ++j) {
                    state.chars.push_back({i + j,
                        grouped_character_progress(group, timing, j, time)});
                }
            } else {
                for (std::size_t j = 0; j < safe_span; ++j) {
                    const auto& item = line.chars[i + j];
                    state.chars.push_back({i + j, temporal_progress(
                        time, item.start_time, item.end_time)});
                }
            }
            i += safe_span;
        }
    }
    return frame;
}

} // namespace kirakara::show
