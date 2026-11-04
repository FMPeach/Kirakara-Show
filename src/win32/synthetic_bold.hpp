#pragma once

namespace kirakara::show::win32::detail {

// Skia's size-dependent fake-bold pen width, in logical pixels (both sides).
// https://github.com/google/skia/blob/main/src/core/SkTextFormatParams.h
constexpr float skia_fake_bold_stroke(float size) noexcept {
    constexpr float small_scale = 1.0F / 24.0F;
    constexpr float large_scale = 1.0F / 32.0F;
    if (size <= 9.0F) return size * small_scale;
    if (size >= 36.0F) return size * large_scale;
    const float t = (size - 9.0F) / (36.0F - 9.0F);
    return size * (small_scale + (large_scale - small_scale) * t);
}

constexpr float small_text_skia_factor(float size) noexcept {
    if (size <= 28.0F) return 1.0F;
    if (size >= 30.0F) return 0.0F;
    return (30.0F - size) / 2.0F;
}

constexpr float small_text_skia_stroke(float size) noexcept {
    return skia_fake_bold_stroke(size) * small_text_skia_factor(size);
}

} // namespace kirakara::show::win32::detail
