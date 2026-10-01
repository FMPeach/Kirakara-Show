#include "cast_overlay_renderer.h"

#include "cast_pipeline_diagnostics.h"
#include "kirakara/show/timeline.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct DesignBand {
    float top{std::numeric_limits<float>::infinity()};
    float bottom{-std::numeric_limits<float>::infinity()};

    [[nodiscard]] bool empty() const noexcept {
        return !(bottom > top);
    }

    void include(float requested_top, float requested_bottom) noexcept {
        if (!std::isfinite(requested_top)
                || !std::isfinite(requested_bottom)
                || !(requested_bottom > requested_top)) {
            return;
        }
        top = std::min(top, requested_top);
        bottom = std::max(bottom, requested_bottom);
    }
};

void hash_value(std::uint64_t& hash, std::uint64_t value) noexcept {
    hash ^= value;
    hash *= kFnvPrime;
}

bool line_has_visible_content(
        const kirakara::show::LineState& line) noexcept {
    if (line.opacity > 0.0 && !line.chars.empty()) return true;
    if (!line.indicator_visible) return false;
    for (const auto opacity : line.indicator_opacity) {
        if (opacity > 0.0) return true;
    }
    return false;
}

bool frame_has_visible_lyrics(
        const kirakara::show::FrameState& frame) noexcept {
    for (std::size_t slot = 0; slot < frame.has_line.size(); ++slot) {
        if (frame.has_line[slot]
                && line_has_visible_content(frame.slots[slot])) {
            return true;
        }
    }
    return false;
}

std::uint64_t overlay_signature(
        const kirakara::show::FrameState& frame,
        float title_opacity,
        bool title_active,
        bool lyrics_active,
        std::uint64_t content_revision) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_value(hash, content_revision);
    hash_value(hash, title_active ? 1U : 0U);
    hash_value(hash, lyrics_active ? 1U : 0U);
    hash_value(hash, std::bit_cast<std::uint32_t>(title_opacity));
    if (!lyrics_active) return hash;
    for (std::size_t slot = 0; slot < frame.has_line.size(); ++slot) {
        hash_value(hash, frame.has_line[slot] ? 1U : 0U);
        if (!frame.has_line[slot]) continue;
        const auto& line = frame.slots[slot];
        hash_value(hash, line.source_line);
        hash_value(hash, std::bit_cast<std::uint64_t>(line.opacity));
        hash_value(hash, line.indicator_visible ? 1U : 0U);
        for (const auto opacity : line.indicator_opacity) {
            hash_value(hash, std::bit_cast<std::uint64_t>(opacity));
        }
        for (const auto& character : line.chars) {
            hash_value(hash, character.char_index);
            hash_value(hash, std::bit_cast<std::uint64_t>(
                character.progress));
        }
    }
    return hash;
}

CastOverlayRect union_rect(
        const CastOverlayRect& left,
        const CastOverlayRect& right) noexcept {
    if (left.empty()) return right;
    if (right.empty()) return left;
    const auto x1 = std::min(left.x, right.x);
    const auto y1 = std::min(left.y, right.y);
    const auto x2 = std::max(left.x + left.width, right.x + right.width);
    const auto y2 = std::max(left.y + left.height, right.y + right.height);
    return {x1, y1, x2 - x1, y2 - y1};
}

void include_title_band(
        DesignBand& band,
        const kirakara::show::SongTitleConfig& title) noexcept {
    for (const auto& block : title.blocks) {
        const auto& style = block.style;
        const auto font_size = std::max(1.0F, std::abs(style.font_size));
        const auto padding = std::max(
            8.0F, std::abs(style.stroke_width) * 2.2F + 4.0F);
        for (std::size_t line = 0; line < block.lines.size(); ++line) {
            if (block.lines[line].empty()) continue;
            const auto css_y = style.y + static_cast<float>(line)
                * (style.font_size + style.line_spacing);
            // The renderer measures the exact DirectWrite line box. This
            // intentionally wider envelope also covers unusual font ascent /
            // descent without making the compositor depend on DWrite.
            band.include(css_y - font_size - padding,
                css_y + font_size * 2.0F + padding);
        }
    }
}

float maximum_text_stroke(
        const kirakara::show::RenderStyle& style) noexcept {
    auto value = std::max({std::abs(style.stroke_width),
        std::abs(style.ruby_stroke_width),
        std::abs(style.ruby2_stroke_width)});
    for (const auto& [name, profile] : style.character_profiles) {
        static_cast<void>(name);
        value = std::max(value, std::abs(profile.stroke_width));
    }
    return value;
}

void include_lyric_band(
        DesignBand& band,
        const kirakara::show::LineState& state,
        std::size_t slot,
        const kirakara::show::EngineConfig& engine,
        const kirakara::show::RenderStyle& style) noexcept {
    const auto css_y = slot == 0 ? style.line1_y : style.line2_y;
    const auto font_size = std::max(1.0F, std::abs(style.font_size));
    const auto ruby_size = std::max(0.0F, std::abs(style.ruby_size));
    const auto ruby2_size = std::max(0.0F, std::abs(style.ruby2_size));
    const auto padding = std::max(
        10.0F, maximum_text_stroke(style) * 3.0F + 6.0F);

    band.include(css_y - font_size * 0.75F - padding,
        css_y + font_size * 1.75F + padding);
    if (ruby_size > 0.0F) {
        const auto ruby_top = css_y - style.ruby_above_offset
            - ruby_size * 1.6F;
        band.include(ruby_top - ruby_size * 0.75F - padding,
            ruby_top + ruby_size * 2.0F + padding);
    }
    if (ruby2_size > 0.0F) {
        const auto ruby2_top = css_y + font_size * 1.2F
            + style.ruby_below_offset;
        band.include(ruby2_top - ruby2_size * 0.75F - padding,
            ruby2_top + ruby2_size * 2.0F + padding);
    }
    if (state.indicator_visible) {
        const auto indicator_top = css_y
            - (style.ruby_size + style.ruby_above_offset
                + engine.indicator.offset_y)
            - engine.indicator.size;
        band.include(indicator_top - padding,
            indicator_top + engine.indicator.size + padding);
    }
    if (!style.show_role_labels) return;
    for (const auto& [name, profile] : style.character_profiles) {
        static_cast<void>(name);
        if (!profile.show_label) continue;
        const auto label_size = std::max(1.0F,
            std::abs(style.font_size * profile.label_scale / 100.0F));
        const auto label_top = css_y + font_size - label_size
            + profile.label_offset_y;
        band.include(label_top - padding,
            label_top + label_size + padding);
    }
}

CastOverlayRect physical_band(
        const DesignBand& band,
        std::uint32_t width,
        std::uint32_t height) noexcept {
    if (band.empty() || width == 0 || height == 0) return {};
    const auto scale = std::min(
        static_cast<float>(width) / 1280.0F,
        static_cast<float>(height) / 720.0F);
    const auto offset_y = (static_cast<float>(height) - 720.0F * scale)
        * 0.5F;
    auto top = static_cast<std::int64_t>(std::floor(
        offset_y + band.top * scale)) - 2;
    auto bottom = static_cast<std::int64_t>(std::ceil(
        offset_y + band.bottom * scale)) + 2;
    top = std::clamp<std::int64_t>(top, 0, height);
    bottom = std::clamp<std::int64_t>(bottom, 0, height);
    top &= ~std::int64_t{1};
    bottom = std::min<std::int64_t>(height, (bottom + 1) & ~std::int64_t{1});
    if (bottom <= top) return {};
    return {0, static_cast<std::uint32_t>(top), width,
        static_cast<std::uint32_t>(bottom - top)};
}

CastOverlayRect overlay_coverage(
        const kirakara::show::FrameState& lyrics,
        bool title_active,
        bool lyrics_active,
        const kirakara::show::SongTitleConfig& title,
        const kirakara::show::EngineConfig& engine,
        const kirakara::show::RenderStyle& style,
        std::uint32_t width,
        std::uint32_t height) noexcept {
    DesignBand band;
    if (title_active) include_title_band(band, title);
    if (lyrics_active) {
        for (std::size_t slot = 0; slot < lyrics.has_line.size(); ++slot) {
            if (lyrics.has_line[slot]
                    && line_has_visible_content(lyrics.slots[slot])) {
                include_lyric_band(
                    band, lyrics.slots[slot], slot, engine, style);
            }
        }
    }
    return physical_band(band, width, height);
}

}  // namespace

struct CastOverlayRenderer::Impl {
    kirakara::show::win32::D3D11TextureSurface surface;
    CastPipelineDiagnostics* diagnostics{};
    CastOverlayFrame frame;
    std::uint64_t last_signature{};
    bool signature_valid{};
    CastOverlayRect previous_coverage;

    bool clear_transparent() {
        surface.begin_draw();
        surface.clear({0.0F, 0.0F, 0.0F, 0.0F});
        return surface.end_draw();
    }

    bool clear_transparent(const CastOverlayRect& rect) {
        if (rect.empty()) return true;
        surface.begin_draw();
        const auto cleared = surface.clear_rect(
            static_cast<float>(rect.x),
            static_cast<float>(rect.y),
            static_cast<float>(rect.x + rect.width),
            static_cast<float>(rect.y + rect.height));
        const auto ended = surface.end_draw();
        return cleared && ended;
    }

    bool configure(
            void* device,
            std::uint32_t width,
            std::uint32_t height,
            CastPipelineDiagnostics* requested_diagnostics) {
        reset();
        if (!device || width == 0 || height == 0
                || !surface.resize_on_device(device, width, height)
                || !clear_transparent()) {
            reset();
            return false;
        }
        surface.flush_d2d();
        diagnostics = requested_diagnostics;
        frame.texture = surface.native_texture();
        frame.width = width;
        frame.height = height;
        frame.content_version = 1;
        frame.empty = true;
        frame.changed = true;
        frame.dirty_rect = {0, 0, width, height};
        return true;
    }

    void reset() noexcept {
        surface = {};
        diagnostics = nullptr;
        frame = {};
        last_signature = 0;
        signature_valid = false;
        previous_coverage = {};
    }

    bool render(
            const kirakara::show::PreparedDocument& document,
            const kirakara::show::SongTitleConfig& title,
            kirakara::show::Seconds current_time,
            const kirakara::show::EngineConfig& engine,
            const kirakara::show::RenderStyle& style,
            std::uint64_t content_revision,
            kirakara::show::win32::LyricRenderer& lyric_renderer,
            kirakara::show::win32::SongTitleRenderer* title_renderer) {
        if (!frame.texture || frame.width == 0 || frame.height == 0) {
            return false;
        }
        const auto lyric_frame = kirakara::show::evaluate_frame(
            document, current_time, engine);
        const auto title_opacity = title_renderer
            ? kirakara::show::song_title_text_opacity(
                current_time, title, engine.fade_duration)
            : 0.0F;
        const bool title_active = title_renderer
            && kirakara::show::song_title_has_drawable_content(title)
            && title_opacity > 0.0F;
        const bool lyrics_active = frame_has_visible_lyrics(lyric_frame);
        const bool empty = !title_active && !lyrics_active;
        const auto signature = overlay_signature(
            lyric_frame, title_opacity, title_active, lyrics_active,
            content_revision);
        const auto current_coverage = overlay_coverage(
            lyric_frame, title_active, lyrics_active, title, engine, style,
            frame.width, frame.height);

        frame.changed = false;
        frame.dirty_rect = {};
        frame.coverage_rect = current_coverage;
        frame.title_drawn = title_active;
        frame.lyrics_drawn = lyrics_active;
        if (signature_valid && signature == last_signature
                && frame.empty == empty) {
            return true;
        }
        if (empty && frame.empty) {
            last_signature = signature;
            signature_valid = true;
            return true;
        }

        const auto started = diagnostics
            ? CastPipelineDiagnostics::qpc_now() : 0;
        const auto dirty_rect = union_rect(
            previous_coverage, current_coverage);
        if (!clear_transparent(dirty_rect)) {
            signature_valid = false;
            previous_coverage = {0, 0, frame.width, frame.height};
            return false;
        }
        if (title_active) {
            if (!title_renderer->attach_native_target(
                    surface.native_render_target())
                    || !title_renderer->render(
                        title, current_time, engine.fade_duration)) {
                return false;
            }
        }
        if (lyrics_active) {
            if (!lyric_renderer.attach_native_target(
                    surface.native_render_target())
                    || !lyric_renderer.render_overlay_frame(
                        document, lyric_frame, current_time, engine, style)) {
                return false;
            }
        }
        surface.flush_d2d();

        ++frame.content_version;
        frame.empty = empty;
        frame.changed = true;
        frame.dirty_rect = dirty_rect;
        previous_coverage = current_coverage;
        last_signature = signature;
        signature_valid = true;
        if (diagnostics) {
            diagnostics->record_duration(
                CastPipelineEvent::overlay_draw,
                frame.content_version,
                started,
                CastPipelineDiagnostics::qpc_now(),
                content_revision,
                (title_active ? 1 : 0) | (lyrics_active ? 2 : 0));
        }
        return true;
    }
};

CastOverlayRenderer::CastOverlayRenderer()
    : impl_(std::make_unique<Impl>()) {}

CastOverlayRenderer::~CastOverlayRenderer() = default;

bool CastOverlayRenderer::configure(
        void* d3d11_device,
        std::uint32_t width,
        std::uint32_t height,
        CastPipelineDiagnostics* diagnostics) {
    return impl_ && impl_->configure(
        d3d11_device, width, height, diagnostics);
}

void CastOverlayRenderer::reset() noexcept {
    if (impl_) impl_->reset();
}

bool CastOverlayRenderer::render(
        const kirakara::show::PreparedDocument& document,
        const kirakara::show::SongTitleConfig& title,
        kirakara::show::Seconds current_time,
        const kirakara::show::EngineConfig& engine,
        const kirakara::show::RenderStyle& style,
        std::uint64_t content_revision,
        kirakara::show::win32::LyricRenderer& lyric_renderer,
        kirakara::show::win32::SongTitleRenderer* title_renderer) {
    return impl_ && impl_->render(document, title, current_time,
        engine, style, content_revision, lyric_renderer, title_renderer);
}

const CastOverlayFrame& CastOverlayRenderer::frame() const noexcept {
    static const CastOverlayFrame empty;
    return impl_ ? impl_->frame : empty;
}
