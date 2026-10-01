#include "kirakara/show/project_parser.hpp"

#include "kirakara/show/lrc_parser.hpp"

#include <cctype>
#include <iterator>
#include <optional>
#include <string>

namespace kirakara::show {
namespace {

struct BlockRange {
    std::size_t begin{};
    std::size_t object_begin{};
    std::size_t end{};
};

bool identifier_boundary(char value) {
    const auto byte = static_cast<unsigned char>(value);
    return !std::isalnum(byte) && value != '_';
}

bool metadata_line_prefix(std::string_view text, std::size_t position) {
    auto line_begin = position;
    while (line_begin > 0 && text[line_begin - 1] != '\n'
            && text[line_begin - 1] != '\r') {
        --line_begin;
    }
    auto cursor = line_begin;
    if (cursor == 0 && text.size() >= 3
            && static_cast<unsigned char>(text[0]) == 0xefU
            && static_cast<unsigned char>(text[1]) == 0xbbU
            && static_cast<unsigned char>(text[2]) == 0xbfU) {
        cursor = 3;
    }
    while (cursor < position) {
        if (!std::isspace(static_cast<unsigned char>(text[cursor]))) {
            return false;
        }
        ++cursor;
    }
    return true;
}

std::optional<BlockRange> find_block(
        std::string_view text, std::string_view name,
        std::size_t search_from = 0) {
    auto position = text.find(name, search_from);
    while (position != std::string_view::npos) {
        const auto left_ok = position == 0
            || identifier_boundary(text[position - 1]);
        const auto after_name = position + name.size();
        const auto right_ok = after_name >= text.size()
            || identifier_boundary(text[after_name]);
        auto brace = after_name;
        while (brace < text.size()
                && std::isspace(static_cast<unsigned char>(text[brace]))) {
            ++brace;
        }
        if (metadata_line_prefix(text, position)
                && left_ok && right_ok && brace < text.size()
                && text[brace] == '{') {
            bool in_string = false;
            bool escaped = false;
            std::size_t depth = 0;
            for (auto cursor = brace; cursor < text.size(); ++cursor) {
                const auto current = text[cursor];
                if (in_string) {
                    if (escaped) {
                        escaped = false;
                    } else if (current == '\\') {
                        escaped = true;
                    } else if (current == '"') {
                        in_string = false;
                    }
                    continue;
                }
                if (current == '"') {
                    in_string = true;
                } else if (current == '{') {
                    ++depth;
                } else if (current == '}' && --depth == 0) {
                    return BlockRange{position, brace, cursor + 1};
                }
            }
            return std::nullopt;
        }
        position = text.find(name, position + name.size());
    }
    return std::nullopt;
}

void strip_blocks(std::string& text, std::string_view name) {
    while (const auto block = find_block(text, name)) {
        text.erase(block->begin, block->end - block->begin);
    }
}

} // namespace

ProjectParseResult parse_project_text(
        std::string_view utf8, AppConfig base_config) {
    ProjectParseResult result;
    result.config = std::move(base_config);
    std::string lyric_text(utf8);

    if (const auto config_block = find_block(lyric_text, "config")) {
        result.has_config = true;
        const auto json = std::string_view(lyric_text).substr(
            config_block->object_begin,
            config_block->end - config_block->object_begin);
        result.config_valid = load_app_config_json(json, result.config);
        if (!result.config_valid) {
            result.warnings.emplace_back("invalid KRL config block");
        }
        lyric_text.erase(config_block->begin,
            config_block->end - config_block->begin);
    }

    // Role blocks are project metadata. Character profiles have already been
    // normalized through the config object and must not reach the LRC parser.
    strip_blocks(lyric_text, "role");
    auto parsed = parse_lrc(lyric_text, result.config.engine);
    result.document = std::move(parsed.document);
    result.warnings.insert(result.warnings.end(),
        std::make_move_iterator(parsed.warnings.begin()),
        std::make_move_iterator(parsed.warnings.end()));
    return result;
}

} // namespace kirakara::show
