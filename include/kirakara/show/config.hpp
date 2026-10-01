#pragma once

#include "kirakara/show/types.hpp"
#include "kirakara/show/render_style.hpp"
#include "kirakara/show/song_title.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace kirakara::show {

struct AppConfig {
    EngineConfig engine;
    RenderStyle style;
    SongTitleConfig song_title{default_song_title_config()};
    std::wstring font_family{L"MotoyaLMaru W3 Mono"};
    std::vector<std::wstring> font_families{font_family};
};

[[nodiscard]] bool load_app_config(std::wstring_view path,
    AppConfig& config);
[[nodiscard]] bool load_app_config_json(std::string_view json,
    AppConfig& config);
[[nodiscard]] AppConfig default_app_config();

} // namespace kirakara::show
