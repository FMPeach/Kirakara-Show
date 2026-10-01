#include "kirakara/show/win32/song_title_renderer.hpp"

#include <windows.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kirakara::show::win32 {
namespace {

template <typename T>
void release(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

bool same_d2d_resource_domain(
        ID2D1RenderTarget* first, ID2D1RenderTarget* second) {
    if (!first || !second) return false;
    ID2D1DeviceContext* first_context{};
    ID2D1DeviceContext* second_context{};
    ID2D1Device* first_device{};
    ID2D1Device* second_device{};
    if (SUCCEEDED(first->QueryInterface(IID_PPV_ARGS(&first_context)))) {
        first_context->GetDevice(&first_device);
    }
    if (SUCCEEDED(second->QueryInterface(IID_PPV_ARGS(&second_context)))) {
        second_context->GetDevice(&second_device);
    }
    const bool same = first_device && first_device == second_device;
    release(second_device);
    release(first_device);
    release(second_context);
    release(first_context);
    return same;
}

std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) return {};
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return L"?";
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::vector<std::string> utf8_characters(std::string_view text) {
    std::vector<std::string> result;
    for (std::size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index]);
        const std::size_t count = (lead & 0x80U) == 0 ? 1
            : (lead & 0xe0U) == 0xc0U ? 2
            : (lead & 0xf0U) == 0xe0U ? 3 : 4;
        const auto safe = std::min(count, text.size() - index);
        result.emplace_back(text.substr(index, safe));
        index += safe;
    }
    return result;
}

D2D1_COLOR_F color_from_packed(PackedColor value, float opacity = 1.0F) {
    return D2D1::ColorF(
        static_cast<float>(value & 0xffU) / 255.0F,
        static_cast<float>((value >> 8U) & 0xffU) / 255.0F,
        static_cast<float>((value >> 16U) & 0xffU) / 255.0F,
        opacity);
}

std::wstring lower_ascii(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return ch >= L'A' && ch <= L'Z'
            ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
    });
    return value;
}

void append_unique(std::vector<std::wstring>& values, std::wstring value) {
    if (!value.empty()
            && std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(std::move(value));
    }
}

std::vector<std::wstring> expanded_font_families(
        const std::vector<std::wstring>& requested) {
    std::vector<std::wstring> result;
    for (const auto& family : requested) {
        const auto generic = lower_ascii(family);
        if (generic == L"sans-serif") {
            append_unique(result, L"Microsoft YaHei UI");
            append_unique(result, L"Yu Gothic UI");
            append_unique(result, L"Meiryo");
            append_unique(result, L"Segoe UI");
        } else if (generic == L"serif") {
            append_unique(result, L"Yu Mincho");
            append_unique(result, L"MS Mincho");
            append_unique(result, L"SimSun");
        } else if (generic == L"monospace") {
            append_unique(result, L"Cascadia Mono");
            append_unique(result, L"Consolas");
            append_unique(result, L"MS Gothic");
        } else {
            append_unique(result, family);
        }
    }
    append_unique(result, L"Microsoft YaHei UI");
    append_unique(result, L"Yu Gothic UI");
    append_unique(result, L"Meiryo");
    append_unique(result, L"Segoe UI");
    return result;
}

std::wstring style_key(const SongTitleStyle& style) {
    std::wstring key;
    for (const auto& family : style.font_families) {
        key += family;
        key.push_back(L'\x1f');
    }
    key += std::to_wstring(static_cast<int>(std::lround(
        style.font_size * 100.0F)));
    key += style.font_bold ? L"|b" : L"|n";
    return key;
}

class GeometryCollector final : public IDWriteTextRenderer {
public:
    explicit GeometryCollector(ID2D1Factory* factory) : factory_(factory) {}

    ~GeometryCollector() {
        for (auto*& geometry : geometries_) release(geometry);
    }

    std::vector<ID2D1Geometry*> take() {
        return std::exchange(geometries_, {});
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWritePixelSnapping)
                || iid == __uuidof(IDWriteTextRenderer)) {
            *object = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const auto value = static_cast<ULONG>(InterlockedDecrement(&references_));
        if (value == 0) delete this;
        return value;
    }

    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* value) override {
        if (!value) return E_POINTER;
        *value = FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*,
            DWRITE_MATRIX* transform) override {
        if (!transform) return E_POINTER;
        *transform = DWRITE_MATRIX{1, 0, 0, 1, 0, 0};
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* value) override {
        if (!value) return E_POINTER;
        *value = 1.0F;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT baseline_x,
            FLOAT baseline_y, DWRITE_MEASURING_MODE,
            const DWRITE_GLYPH_RUN* run,
            const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        if (!run || !run->fontFace) return E_INVALIDARG;
        ID2D1PathGeometry* path{};
        ID2D1GeometrySink* sink{};
        ID2D1TransformedGeometry* positioned{};
        auto result = factory_->CreatePathGeometry(&path);
        if (SUCCEEDED(result)) result = path->Open(&sink);
        if (SUCCEEDED(result)) {
            result = run->fontFace->GetGlyphRunOutline(run->fontEmSize,
                run->glyphIndices, run->glyphAdvances, run->glyphOffsets,
                run->glyphCount, run->isSideways,
                (run->bidiLevel & 1U) != 0, sink);
        }
        if (sink) {
            const auto closed = sink->Close();
            if (SUCCEEDED(result)) result = closed;
        }
        if (SUCCEEDED(result)) {
            result = factory_->CreateTransformedGeometry(path,
                D2D1::Matrix3x2F::Translation(baseline_x, baseline_y),
                &positioned);
        }
        if (SUCCEEDED(result) && positioned) {
            geometries_.push_back(positioned);
            positioned = nullptr;
        }
        release(positioned);
        release(sink);
        release(path);
        return result;
    }

    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT,
        const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT,
        const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT,
        IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return E_NOTIMPL; }

private:
    LONG references_{1};
    ID2D1Factory* factory_{};
    std::vector<ID2D1Geometry*> geometries_;
};

struct CachedGlyph {
    std::vector<ID2D1Geometry*> geometry;
    float width{1.0F};
};

struct LineMetrics {
    float baseline{};
    float height{};
};

} // namespace

struct SongTitleRenderer::Impl {
    IDWriteFactory* dwrite{};
    ID2D1Factory* d2d{};
    ID2D1RenderTarget* target{};
    ID2D1StrokeStyle* round_stroke{};
    std::unordered_map<std::wstring, CachedGlyph> glyph_cache;
    std::unordered_map<std::wstring, LineMetrics> line_metrics_cache;

    // Whole-block bitmap cache: a title block is static text, so rasterize
    // each block once to an offscreen bitmap and draw the cached bitmap per
    // frame. Removes per-frame vector geometry (DrawGeometry/FillGeometry
    // per glyph) and per-frame solid-brush creation, which together cost
    // roughly 40% of the frame budget in the unlocked video_demo path.
    //
    // Validity: a cached bitmap stays valid while (a) block text/style and
    // (b) the design->target scale stay unchanged. The cache is cleared on
    // target re-create (discard_target) and capped so stale titles from
    // other songs cannot accumulate. If a future build animates block
    // transforms per frame, key the cache by scale or apply a freshness
    // rule (e.g. re-rasterize at least every 10s) so a stale bitmap is
    // never shown -- the per-frame transform cost of DrawBitmap is tiny, so
    // position/opacity changes do NOT invalidate the bitmap itself.
    //
    // FUTURE (higher fidelity): mirror the lyric renderer's glyph-mask
    // scheme -- pre-render each glyph to a white 2x-supersampled
    // ID2D1Bitmap and per-frame FillOpacityMask. That keeps per-character
    // animation/coloring, which this whole-block bitmap cannot provide.
    struct CachedBlockBitmap {
        ID2D1Bitmap* bitmap{};
        float left{};   // content box in 1280x720 design coords
        float top{};
        float width{};
        float height{};
    };
    std::unordered_map<std::wstring, CachedBlockBitmap> block_bitmap_cache;
    static constexpr std::size_t kMaxBlockBitmapCache = 16;

    struct MeasuredBlock {
        struct Line {
            std::vector<const CachedGlyph*> glyphs;
            float width{};
            float left{};
            float top{};
        };
        std::vector<Line> lines;
        float left{};
        float top{};
        float width{};
        float height{};
    };

    ~Impl() { shutdown(); }

    void clear_geometry_cache() {
        for (auto& [key, glyph] : glyph_cache) {
            static_cast<void>(key);
            for (auto*& geometry : glyph.geometry) release(geometry);
        }
        glyph_cache.clear();
    }

    void clear_block_bitmap_cache() {
        for (auto& [key, entry] : block_bitmap_cache) {
            static_cast<void>(key);
            release(entry.bitmap);
        }
        block_bitmap_cache.clear();
    }

    void discard_target() {
        clear_geometry_cache();
        clear_block_bitmap_cache();
        release(round_stroke);
        release(target);
        release(d2d);
    }

    void shutdown() {
        discard_target();
        line_metrics_cache.clear();
        release(dwrite);
    }

    bool initialize() {
        if (dwrite) return true;
        const auto dwrite_result = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&dwrite));
        if (FAILED(dwrite_result)) {
            shutdown();
            return false;
        }
        return true;
    }

    void create_round_stroke() {
        release(round_stroke);
        if (!d2d) return;
        auto properties = D2D1::StrokeStyleProperties();
        properties.startCap = D2D1_CAP_STYLE_ROUND;
        properties.endCap = D2D1_CAP_STYLE_ROUND;
        properties.dashCap = D2D1_CAP_STYLE_ROUND;
        properties.lineJoin = D2D1_LINE_JOIN_ROUND;
        d2d->CreateStrokeStyle(properties, nullptr, 0, &round_stroke);
    }

    bool attach(ID2D1RenderTarget* external) {
        if (!external) return false;
        if (external == target) return true;
        if (same_d2d_resource_domain(target, external)) {
            external->AddRef();
            release(target);
            target = external;
            return true;
        }
        discard_target();
        external->AddRef();
        target = external;
        external->GetFactory(&d2d);
        create_round_stroke();
        return d2d && round_stroke;
    }

    IDWriteTextFormat* format(const SongTitleStyle& style) {
        IDWriteTextFormat* result{};
        for (const auto& family : expanded_font_families(style.font_families)) {
            if (SUCCEEDED(dwrite->CreateTextFormat(family.c_str(), nullptr,
                    style.font_bold ? DWRITE_FONT_WEIGHT_BOLD
                                    : DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    style.font_size, L"zh-CN", &result)) && result) {
                break;
            }
        }
        if (result) {
            result->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            result->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            result->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
        return result;
    }

    IDWriteTextLayout* layout(std::string_view text,
            const SongTitleStyle& style) {
        auto* text_format = format(style);
        if (!text_format) return nullptr;
        const auto wide = utf8_to_wide(text);
        IDWriteTextLayout* result{};
        dwrite->CreateTextLayout(wide.c_str(), static_cast<UINT32>(wide.size()),
            text_format, 4096.0F, style.font_size * 2.0F, &result);
        release(text_format);
        return result;
    }

    const CachedGlyph* glyph(std::string_view text,
            const SongTitleStyle& style) {
        auto key = style_key(style);
        key.push_back(L'|');
        key += utf8_to_wide(text);
        if (const auto found = glyph_cache.find(key);
                found != glyph_cache.end()) {
            return &found->second;
        }

        auto* text_layout = layout(text, style);
        if (!text_layout) return nullptr;
        DWRITE_TEXT_METRICS metrics{};
        text_layout->GetMetrics(&metrics);
        CachedGlyph created;
        created.width = std::max(1.0F, metrics.widthIncludingTrailingWhitespace);
        auto* collector = new GeometryCollector(d2d);
        const auto result = text_layout->Draw(nullptr, collector, 0.0F, 0.0F);
        release(text_layout);
        if (SUCCEEDED(result)) created.geometry = collector->take();
        collector->Release();
        return &glyph_cache.emplace(std::move(key), std::move(created))
            .first->second;
    }

    LineMetrics natural_line_metrics(const SongTitleStyle& style) {
        auto key = style_key(style) + L"|line";
        if (const auto found = line_metrics_cache.find(key);
                found != line_metrics_cache.end()) {
            return found->second;
        }
        LineMetrics result{style.font_size * 0.88F, style.font_size};
        auto* text_layout = layout("M", style);
        if (text_layout) {
            DWRITE_LINE_METRICS metrics{};
            UINT32 count{};
            if (SUCCEEDED(text_layout->GetLineMetrics(&metrics, 1, &count))
                    && count > 0 && metrics.height > 0.0F) {
                result = {metrics.baseline, metrics.height};
            }
            release(text_layout);
        }
        line_metrics_cache.emplace(std::move(key), result);
        return result;
    }

    float css_layout_top(float css_top, const SongTitleStyle& style) {
        const auto natural = natural_line_metrics(style);
        return css_top + (style.font_size - natural.height) * 0.5F;
    }

    // Measures every line of a block in 1280x720 design coordinates: glyph
    // pointers (cached), per-line left/top and the block bounding box.
    MeasuredBlock measure_block(const SongTitleBlock& block) {
        MeasuredBlock result;
        struct RawLine {
            std::vector<const CachedGlyph*> glyphs;
            float width{};
        };
        std::vector<RawLine> raws;
        raws.reserve(block.lines.size());
        float paragraph_width = 0.0F;
        for (const auto& line : block.lines) {
            RawLine raw;
            const auto characters = utf8_characters(line);
            raw.glyphs.reserve(characters.size());
            for (const auto& character : characters) {
                const auto* cached = glyph(character, block.style);
                if (!cached) continue;
                raw.glyphs.push_back(cached);
                raw.width += cached->width;
            }
            if (raw.glyphs.size() > 1) {
                raw.width += static_cast<float>(raw.glyphs.size() - 1)
                    * block.style.letter_spacing;
            }
            paragraph_width = std::max(paragraph_width, raw.width);
            raws.push_back(std::move(raw));
        }
        if (raws.empty()) return result;

        float min_left = std::numeric_limits<float>::max();
        float max_right = -std::numeric_limits<float>::max();
        for (std::size_t line_index = 0; line_index < raws.size();
                ++line_index) {
            MeasuredBlock::Line line;
            line.glyphs = std::move(raws[line_index].glyphs);
            line.width = raws[line_index].width;
            line.left = song_title_line_start_x(block.style.align,
                block.style.x, line.width, paragraph_width);
            const auto css_top = block.style.y
                + static_cast<float>(line_index)
                    * (block.style.font_size + block.style.line_spacing);
            line.top = css_layout_top(css_top, block.style);
            min_left = std::min(min_left, line.left);
            max_right = std::max(max_right, line.left + line.width);
            result.lines.push_back(std::move(line));
        }
        const auto natural = natural_line_metrics(block.style);
        float min_top = std::numeric_limits<float>::max();
        float max_bottom = -std::numeric_limits<float>::max();
        for (const auto& line : result.lines) {
            min_top = std::min(min_top, line.top);
            max_bottom = std::max(max_bottom, line.top + natural.height);
        }
        result.left = min_left;
        result.top = min_top;
        result.width = std::max(0.0F, max_right - min_left);
        result.height = std::max(0.0F, max_bottom - min_top);
        return result;
    }

    // Rasterizes a whole block once to an offscreen bitmap (2x supersampled,
    // premultiplied alpha, transparent padding around the stroke). The block
    // is drawn with its measured layout, so the per-frame path is a single
    // DrawBitmap with per-frame opacity.
    bool render_block_bitmap(const MeasuredBlock& measured,
            const SongTitleBlock& block, CachedBlockBitmap& out) {
        constexpr float supersample = 2.0F;
        const float pad = block.style.stroke_width * 2.2F + 4.0F;
        const float left = std::floor(measured.left - pad * 0.5F);
        const float top = std::floor(measured.top - pad * 0.5F);
        const float right = std::ceil(
            measured.left + measured.width + pad * 0.5F);
        const float bottom = std::ceil(
            measured.top + measured.height + pad * 0.5F);
        const float bw = std::max(1.0F, right - left);
        const float bh = std::max(1.0F, bottom - top);
        const auto size = D2D1::SizeF(bw, bh);
        const auto pixel_size = D2D1::SizeU(
            static_cast<UINT32>(std::lround(bw * supersample)),
            static_cast<UINT32>(std::lround(bh * supersample)));
        const auto pf = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1_ALPHA_MODE_PREMULTIPLIED);
        ID2D1BitmapRenderTarget* offscreen{};
        if (FAILED(target->CreateCompatibleRenderTarget(&size, &pixel_size,
                &pf, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &offscreen))
                || !offscreen) {
            release(offscreen);
            return false;
        }
        offscreen->BeginDraw();
        offscreen->Clear(D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F));
        // Design coordinates map 1:1 to the offscreen DIP space; the 2x
        // supersampling comes from the pixel_size given to
        // CreateCompatibleRenderTarget, NOT from scaling the transform here
        // (scaling would blow the geometry past the DIP bounds and only the
        // top-left quadrant would survive).
        const auto design = D2D1::Matrix3x2F::Translation(-left, -top);
        ID2D1SolidColorBrush* fill{};
        ID2D1SolidColorBrush* stroke{};
        offscreen->CreateSolidColorBrush(
            color_from_packed(block.style.color), &fill);
        if (block.style.stroke_width > 0.0F) {
            offscreen->CreateSolidColorBrush(
                color_from_packed(block.style.stroke_color), &stroke);
        }
        for (const auto& line : measured.lines) {
            float glyph_x = 0.0F;
            for (const auto* cached : line.glyphs) {
                offscreen->SetTransform(design
                    * D2D1::Matrix3x2F::Translation(
                        line.left + glyph_x, line.top));
                for (auto* geometry : cached->geometry) {
                    if (stroke) {
                        offscreen->DrawGeometry(geometry, stroke,
                            block.style.stroke_width * 2.2F, round_stroke);
                    }
                    if (fill) offscreen->FillGeometry(geometry, fill);
                }
                glyph_x += cached->width + block.style.letter_spacing;
            }
        }
        release(stroke);
        release(fill);
        const auto result = offscreen->EndDraw();
        ID2D1Bitmap* bitmap{};
        if (SUCCEEDED(result)) offscreen->GetBitmap(&bitmap);
        release(offscreen);
        if (!bitmap) return false;
        out.bitmap = bitmap;
        out.left = left;
        out.top = top;
        out.width = bw;
        out.height = bh;
        return true;
    }

    bool draw_title(const SongTitleConfig& config, Seconds project_time,
            Seconds lyric_fade_duration, D2D1_SIZE_F target_size) {
        const auto opacity = song_title_text_opacity(project_time, config,
            lyric_fade_duration);
        if (opacity <= 0.0F) return true;

        const auto scale = std::min(target_size.width / 1280.0F,
            target_size.height / 720.0F);
        const auto offset_x = (target_size.width - 1280.0F * scale) * 0.5F;
        const auto offset_y = (target_size.height - 720.0F * scale) * 0.5F;
        const auto design_transform = D2D1::Matrix3x2F(
            scale, 0.0F, 0.0F, scale, offset_x, offset_y);

        for (const auto& block : config.blocks) {
            if (block.lines.empty()) continue;
            std::wstring key = style_key(block.style);
            for (const auto& line : block.lines) {
                key.push_back(L'|');
                key += utf8_to_wide(line);
            }
            CachedBlockBitmap entry;
            if (const auto found = block_bitmap_cache.find(key);
                    found != block_bitmap_cache.end()) {
                entry = found->second;
            } else {
                const auto measured = measure_block(block);
                if (measured.lines.empty()) continue;
                if (!render_block_bitmap(measured, block, entry)) continue;
                if (block_bitmap_cache.size() >= kMaxBlockBitmapCache) {
                    clear_block_bitmap_cache();
                }
                block_bitmap_cache.emplace(std::move(key), entry);
            }
            target->SetTransform(design_transform);
            target->DrawBitmap(entry.bitmap,
                D2D1::RectF(entry.left, entry.top,
                    entry.left + entry.width, entry.top + entry.height),
                opacity,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                D2D1::RectF(0.0F, 0.0F, entry.width, entry.height));
        }
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        return true;
    }

    bool render(const SongTitleConfig& config, Seconds project_time,
            Seconds lyric_fade_duration) {
        if (!target) return false;
        const auto target_size = target->GetSize();
        D2D1_MATRIX_3X2_F previous{};
        target->GetTransform(&previous);
        target->BeginDraw();
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        auto result = draw_title(config, project_time, lyric_fade_duration,
            target_size);
        target->SetTransform(previous);
        const auto end_result = target->EndDraw();
        if (end_result == D2DERR_RECREATE_TARGET) discard_target();
        return result && SUCCEEDED(end_result);
    }
};

SongTitleRenderer::SongTitleRenderer() : impl_(std::make_unique<Impl>()) {}
SongTitleRenderer::~SongTitleRenderer() = default;
SongTitleRenderer::SongTitleRenderer(SongTitleRenderer&&) noexcept = default;
SongTitleRenderer& SongTitleRenderer::operator=(SongTitleRenderer&&) noexcept = default;

bool SongTitleRenderer::initialize() {
    return impl_ && impl_->initialize();
}

void SongTitleRenderer::shutdown() {
    if (impl_) impl_->shutdown();
}

bool SongTitleRenderer::attach_native_target(void* d2d_render_target) {
    return impl_ && impl_->attach(
        static_cast<ID2D1RenderTarget*>(d2d_render_target));
}

bool SongTitleRenderer::render(const SongTitleConfig& config,
        Seconds project_time, Seconds lyric_fade_duration) {
    return impl_ && impl_->render(config, project_time,
        lyric_fade_duration);
}

} // namespace kirakara::show::win32
