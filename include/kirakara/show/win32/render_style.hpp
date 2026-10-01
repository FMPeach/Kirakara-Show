#pragma once

#include "kirakara/show/render_style.hpp"

// Backward-compat: alias cross-platform types into win32 namespace.
namespace kirakara::show::win32 {
using kirakara::show::PackedColor;
using kirakara::show::packed_rgb;
using kirakara::show::kRolePalette;
using kirakara::show::palette_color;
using kirakara::show::TextPaint;
using kirakara::show::CharacterProfile;
using kirakara::show::RenderStyle;
} // namespace kirakara::show::win32
