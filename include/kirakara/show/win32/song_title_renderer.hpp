#pragma once

#include "kirakara/show/song_title.hpp"

#include <memory>

namespace kirakara::show::win32 {

// Direct2D renderer for parser-normalized text blocks. It intentionally knows
// nothing about KRL editor groups, and it preserves existing target pixels.
class SongTitleRenderer {
public:
    SongTitleRenderer();
    ~SongTitleRenderer();

    SongTitleRenderer(const SongTitleRenderer&) = delete;
    SongTitleRenderer& operator=(const SongTitleRenderer&) = delete;
    SongTitleRenderer(SongTitleRenderer&&) noexcept;
    SongTitleRenderer& operator=(SongTitleRenderer&&) noexcept;

    [[nodiscard]] bool initialize();
    void shutdown();
    [[nodiscard]] bool attach_native_target(void* d2d_render_target);
    [[nodiscard]] bool render(const SongTitleConfig& config,
        Seconds project_time, Seconds lyric_fade_duration);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kirakara::show::win32
