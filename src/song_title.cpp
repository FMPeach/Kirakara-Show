#include "kirakara/show/song_title.hpp"

#include <algorithm>
#include <cmath>

namespace kirakara::show {
namespace {

Seconds normalized_duration(Seconds value) noexcept {
    if (!std::isfinite(value)) value = 3.0;
    return std::clamp(value, 3.0, 10.0);
}

float unit_interval(double value) noexcept {
    return static_cast<float>(std::clamp(value, 0.0, 1.0));
}

} // namespace

SongTitleConfig default_song_title_config() {
    return {};
}

SongTitleTimeline song_title_timeline(const SongTitleConfig& config) noexcept {
    const auto duration = normalized_duration(config.duration);
    return {
        config.enabled,
        duration,
        0.0,
        duration,
    };
}

bool song_title_visible(Seconds project_time,
        const SongTitleConfig& config) noexcept {
    const auto timeline = song_title_timeline(config);
    return timeline.enabled && project_time >= timeline.title_start
        && project_time < timeline.title_end;
}

bool song_title_has_drawable_content(
        const SongTitleConfig& config) noexcept {
    if (!config.enabled) return false;
    for (const auto& block : config.blocks) {
        for (const auto& line : block.lines) {
            if (!line.empty()) return true;
        }
    }
    return false;
}

float song_title_text_opacity(Seconds project_time,
        const SongTitleConfig& config,
        Seconds lyric_fade_duration) noexcept {
    const auto timeline = song_title_timeline(config);
    if (!timeline.enabled || project_time < timeline.title_start
            || project_time >= timeline.title_end) {
        return 0.0F;
    }
    if (!config.text_fade) return 1.0F;

    const auto configured = std::isfinite(lyric_fade_duration)
        ? std::max(0.0, lyric_fade_duration) : 0.666;
    const auto fade = std::min(configured, timeline.duration / 2.0);
    if (fade <= 0.0) return 1.0F;
    const auto fade_in = (project_time - timeline.title_start) / fade;
    const auto fade_out = (timeline.title_end - project_time) / fade;
    return unit_interval(std::min(fade_in, fade_out));
}

float song_title_line_start_x(SongTitleAlign align, float anchor_x,
        float line_width, float paragraph_width) noexcept {
    const auto width = std::max(0.0F, line_width);
    const auto block = std::max(width, std::max(0.0F, paragraph_width));
    switch (align) {
    case SongTitleAlign::center_left: return anchor_x - block / 2.0F;
    case SongTitleAlign::center_right: return anchor_x + block / 2.0F - width;
    case SongTitleAlign::center: return anchor_x - width / 2.0F;
    case SongTitleAlign::right: return anchor_x - width;
    case SongTitleAlign::left: return anchor_x;
    }
    return anchor_x;
}

} // namespace kirakara::show
