#pragma once

#include "kirakara/show/render_style.hpp"
#include "kirakara/show/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace kirakara::show {

enum class SongTitleAlign : std::uint8_t {
    left,
    center_left,
    center,
    center_right,
    right,
};

struct SongTitleStyle {
    std::wstring font_family{L"Microsoft YaHei"};
    std::vector<std::wstring> font_families{font_family, L"sans-serif"};
    float font_size{72.0F};
    bool font_bold{true};
    float letter_spacing{4.0F};
    float line_spacing{};
    float x{640.0F};
    float y{170.0F};
    SongTitleAlign align{SongTitleAlign::center};
    PackedColor color{packed_rgb(255, 255, 255)};
    PackedColor stroke_color{packed_rgb(0, 0, 0)};
    float stroke_width{4.0F};
};

// Renderer input only. KRL group names, kinds, ids and row-editing structure
// are flattened by the project parser before they reach the engine.
struct SongTitleBlock {
    std::vector<std::string> lines;
    SongTitleStyle style;
};

struct SongTitleConfig {
    bool enabled{};
    Seconds duration{3.0};
    bool text_fade{true};
    std::vector<SongTitleBlock> blocks;
};

struct SongTitleTimeline {
    bool enabled{};
    Seconds duration{};
    Seconds title_start{};
    Seconds title_end{};
};

[[nodiscard]] SongTitleConfig default_song_title_config();
[[nodiscard]] SongTitleTimeline song_title_timeline(
    const SongTitleConfig& config) noexcept;
[[nodiscard]] bool song_title_visible(Seconds project_time,
    const SongTitleConfig& config) noexcept;
// Draw-scheduling helper only. The host initializes the title engine whenever
// enabled is true; an enabled but empty title then becomes a drawing no-op.
[[nodiscard]] bool song_title_has_drawable_content(
    const SongTitleConfig& config) noexcept;
[[nodiscard]] float song_title_text_opacity(Seconds project_time,
    const SongTitleConfig& config, Seconds lyric_fade_duration) noexcept;
[[nodiscard]] float song_title_line_start_x(SongTitleAlign align,
    float anchor_x, float line_width, float paragraph_width) noexcept;

} // namespace kirakara::show
