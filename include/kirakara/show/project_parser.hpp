#pragma once

#include "kirakara/show/config.hpp"
#include "kirakara/show/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace kirakara::show {

// Backend-neutral result of loading a KRL/LRC text payload. File I/O remains
// the host's responsibility; every host and renderer backend can share this
// normalization step.
struct ProjectParseResult {
    PreparedDocument document;
    AppConfig config{default_app_config()};
    bool has_config{};
    bool config_valid{true};
    std::vector<std::string> warnings;
};

[[nodiscard]] ProjectParseResult parse_project_text(
    std::string_view utf8,
    AppConfig base_config = default_app_config());

} // namespace kirakara::show
