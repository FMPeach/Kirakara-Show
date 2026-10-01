#pragma once

#include "kirakara/show/types.hpp"

#include <cstddef>
#include <span>

namespace kirakara::show {

// Copies and annotates structured lines with the paragraph/slot replacement
// semantics used by the Kirakara Player Demo DOM preview.
[[nodiscard]] PreparedDocument prepare_timeline(
    std::span<const LyricLine> source,
    const EngineConfig& config = {});

[[nodiscard]] double temporal_progress(
    Seconds time,
    Seconds start_time,
    Seconds end_time) noexcept;

// Maps timed ruby syllables across all base characters in a ruby group.
[[nodiscard]] double grouped_character_progress(
    std::span<const LyricChar> group_chars,
    std::span<const TimedText> ruby_timing,
    std::size_t character_index,
    Seconds time) noexcept;

[[nodiscard]] double line_opacity(
    const LyricLine& line,
    Seconds time,
    const EngineConfig& config) noexcept;

[[nodiscard]] std::array<double, 4> indicator_opacities(
    const LyricLine& line,
    Seconds time,
    const EngineConfig& config) noexcept;

[[nodiscard]] FrameState evaluate_frame(
    const PreparedDocument& document,
    Seconds time,
    const EngineConfig& config = {});

} // namespace kirakara::show
