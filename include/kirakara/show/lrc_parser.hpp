#pragma once

#include "kirakara/show/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace kirakara::show {

struct LrcParseResult {
    PreparedDocument document;
    std::vector<std::string> warnings;
};

struct ReferenceMergeResult {
    std::size_t matched_lines{};
    std::size_t attached_groups{};
    std::vector<std::string> warnings;
};

// Compatibility parser for the syntax accepted by Kirakara Player Demo:
// per-character time tags, {base|ruby>ruby2}, @ruby dictionaries, and
// inherited 【role】 / 【roleA+roleB】 tags.
[[nodiscard]] LrcParseResult parse_lrc(
    std::string_view utf8,
    const EngineConfig& config = {});

// Applies an untimed, line-based segmentation/ruby reference such as:
//   ただ　{静|しず}かに
// to an already timed document. Lines are matched in order after whitespace
// normalization; only grouping/ruby metadata is transferred, never timing.
[[nodiscard]] ReferenceMergeResult apply_lyric_reference(
    PreparedDocument& document,
    std::string_view utf8_reference);

} // namespace kirakara::show
