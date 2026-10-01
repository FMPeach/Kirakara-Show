#include "kirakara/show/win32/lyric_renderer.hpp"
#include "kirakara/show/timeline.hpp"

#include <windows.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace kirakara::show;

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
    const auto count = MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return L"?";
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), count);
    return result;
}

std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const auto count = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), count, nullptr, nullptr);
    return result;
}

std::string read_file(const std::wstring& path) {
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

std::wstring filename(const std::wstring& path) {
    return std::filesystem::path(path).filename().wstring();
}

D2D1_COLOR_F color_from_ref(kirakara::show::PackedColor value, float alpha = 1.0F) {
    return D2D1::ColorF(
        static_cast<float>(GetRValue(value)) / 255.0F,
        static_cast<float>(GetGValue(value)) / 255.0F,
        static_cast<float>(GetBValue(value)) / 255.0F,
        alpha);
}

float native_text_stroke_width(float visual_radius) noexcept {
    // DOM compatibility stroke values describe the outward radius produced by
    // text-shadow copies. Direct2D outline strokes are centered on the glyph
    // path, and the fill covers the inner half, so use a doubled pen width for
    // the same visible outside thickness. This applies only to text; indicator
    // circles and other geometry keep their literal stroke width.
    return visual_radius * 2.0F;
}

float native_ruby_font_size(float css_size) noexcept {
    return css_size;
}

float native_upper_ruby_y_adjust() noexcept {
    // Firefox/DirectWrite renders the DOM RubyMask's line-height/padding/margin
    // box about two pixels higher than a direct top = baseTop - size - offset
    // placement. Keep the CSS-compatible size, and calibrate the line box
    // position instead of scaling the glyphs.
    return -8.0F;
}

using TextPaint = kirakara::show::TextPaint;
using CharacterProfile = kirakara::show::CharacterProfile;
using DemoStyle = kirakara::show::RenderStyle;

TextPaint global_paint(const DemoStyle& style) {
    return {style.before, style.after, style.stroke_before,
            style.stroke_after, style.stroke_width};
}

TextPaint role_paint(const std::string& role, const DemoStyle& style) {
    if (!style.role_colors || role.empty()) return global_paint(style);

    const auto it = style.character_profiles.find(role);
    if (it != style.character_profiles.end()) {
        // Known role: profile -> global -> built-in (chain fallback, no palette).
        const auto& p = it->second;
        return {
            p.color_before  ? p.color_before  : style.before,
            p.color_after   ? p.color_after   : style.after,
            p.stroke_before ? p.stroke_before : style.stroke_before,
            p.stroke_after  ? p.stroke_after  : style.stroke_after,
            p.stroke_width > 0 ? p.stroke_width : style.stroke_width,
        };
    }

    // Role NOT in characterProfiles: random palette.
    // Only changes stroke_before and after (same colour); rest stays global.
    const auto c = kirakara::show::palette_color(
        std::string_view{role.data(), role.size()});
    return {
        style.before,        // before: global
        c,                   // after: palette
        c,                   // stroke_before: palette (same as after)
        style.stroke_after,  // stroke_after: global
        style.stroke_width,  // stroke_width: global
    };
}

class OutlineTextRenderer final : public IDWriteTextRenderer {
public:
    OutlineTextRenderer(ID2D1Factory* factory, ID2D1RenderTarget* target,
        ID2D1Brush* fill, ID2D1Brush* stroke, float stroke_width)
        : factory_(factory), target_(target), fill_(fill), stroke_(stroke),
          stroke_width_(stroke_width) {
        D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties();
        properties.startCap = D2D1_CAP_STYLE_ROUND;
        properties.endCap = D2D1_CAP_STYLE_ROUND;
        properties.dashCap = D2D1_CAP_STYLE_ROUND;
        properties.lineJoin = D2D1_LINE_JOIN_ROUND;
        factory_->CreateStrokeStyle(properties, nullptr, 0, &stroke_style_);
    }

    ~OutlineTextRenderer() { release(stroke_style_); }

    [[nodiscard]] D2D1_RECT_F bounds() const noexcept { return bounds_; }
    [[nodiscard]] bool has_bounds() const noexcept { return has_bounds_; }

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

    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override {
        if (!disabled) return E_POINTER;
        *disabled = FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* matrix) override {
        if (!matrix) return E_POINTER;
        D2D1_MATRIX_3X2_F transform{};
        target_->GetTransform(&transform);
        matrix->m11 = transform._11;
        matrix->m12 = transform._12;
        matrix->m21 = transform._21;
        matrix->m22 = transform._22;
        matrix->dx = transform._31;
        matrix->dy = transform._32;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels_per_dip) override {
        if (!pixels_per_dip) return E_POINTER;
        float dpi_x{}, dpi_y{};
        target_->GetDpi(&dpi_x, &dpi_y);
        *pixels_per_dip = dpi_x / 96.0F;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT baseline_x, FLOAT baseline_y,
        DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run,
        const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        if (!run || !run->fontFace) return E_INVALIDARG;
        ID2D1PathGeometry* path{};
        ID2D1GeometrySink* sink{};
        HRESULT result = factory_->CreatePathGeometry(&path);
        if (SUCCEEDED(result)) result = path->Open(&sink);
        if (SUCCEEDED(result)) {
            result = run->fontFace->GetGlyphRunOutline(run->fontEmSize,
                run->glyphIndices, run->glyphAdvances, run->glyphOffsets,
                run->glyphCount, run->isSideways,
                (run->bidiLevel & 1U) != 0, sink);
        }
        if (sink) {
            const auto close_result = sink->Close();
            if (SUCCEEDED(result)) result = close_result;
        }

        ID2D1TransformedGeometry* positioned{};
        if (SUCCEEDED(result)) {
            result = factory_->CreateTransformedGeometry(path,
                D2D1::Matrix3x2F::Translation(baseline_x, baseline_y), &positioned);
        }
        if (SUCCEEDED(result) && positioned) {
            D2D1_RECT_F glyph_bounds{};
            if (SUCCEEDED(positioned->GetBounds(D2D1::Matrix3x2F::Identity(),
                    &glyph_bounds))) {
                if (!has_bounds_) {
                    bounds_ = glyph_bounds;
                    has_bounds_ = true;
                } else {
                    bounds_.left = std::min(bounds_.left, glyph_bounds.left);
                    bounds_.top = std::min(bounds_.top, glyph_bounds.top);
                    bounds_.right = std::max(bounds_.right, glyph_bounds.right);
                    bounds_.bottom = std::max(bounds_.bottom, glyph_bounds.bottom);
                }
            }
            if (stroke_ && stroke_width_ > 0.0F) {
                target_->DrawGeometry(positioned, stroke_, stroke_width_, stroke_style_);
            }
            if (fill_) target_->FillGeometry(positioned, fill_);
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
    ID2D1RenderTarget* target_{};
    ID2D1Brush* fill_{};
    ID2D1Brush* stroke_{};
    float stroke_width_{};
    ID2D1StrokeStyle* stroke_style_{};
    D2D1_RECT_F bounds_{};
    bool has_bounds_{};
};

class GeometryCollector final : public IDWriteTextRenderer {
public:
    explicit GeometryCollector(ID2D1Factory* factory) : factory_(factory) {}

    ~GeometryCollector() {
        for (auto*& geometry : geometries_) release(geometry);
    }

    std::vector<ID2D1Geometry*> take_geometries() {
        return std::exchange(geometries_, {});
    }

    [[nodiscard]] D2D1_RECT_F bounds() const noexcept { return bounds_; }
    [[nodiscard]] bool has_bounds() const noexcept { return has_bounds_; }

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

    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override {
        *disabled = FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) override {
        *transform = DWRITE_MATRIX{1, 0, 0, 1, 0, 0};
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* value) override {
        *value = 1.0F;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT baseline_x, FLOAT baseline_y,
        DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run,
        const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        ID2D1PathGeometry* path{};
        ID2D1GeometrySink* sink{};
        ID2D1TransformedGeometry* positioned{};
        auto result = factory_->CreatePathGeometry(&path);
        if (SUCCEEDED(result)) result = path->Open(&sink);
        if (SUCCEEDED(result)) {
            result = run->fontFace->GetGlyphRunOutline(run->fontEmSize,
                run->glyphIndices, run->glyphAdvances, run->glyphOffsets,
                run->glyphCount, run->isSideways, run->bidiLevel % 2 != 0, sink);
        }
        if (sink) {
            const auto close_result = sink->Close();
            if (SUCCEEDED(result)) result = close_result;
        }
        if (SUCCEEDED(result)) {
            result = factory_->CreateTransformedGeometry(path,
                D2D1::Matrix3x2F::Translation(baseline_x, baseline_y), &positioned);
        }
        if (SUCCEEDED(result) && positioned) {
            D2D1_RECT_F bounds{};
            if (SUCCEEDED(positioned->GetBounds(nullptr, &bounds))) {
                if (!has_bounds_) {
                    bounds_ = bounds;
                    has_bounds_ = true;
                } else {
                    bounds_.left = std::min(bounds_.left, bounds.left);
                    bounds_.top = std::min(bounds_.top, bounds.top);
                    bounds_.right = std::max(bounds_.right, bounds.right);
                    bounds_.bottom = std::max(bounds_.bottom, bounds.bottom);
                }
            }
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
    D2D1_RECT_F bounds_{};
    bool has_bounds_{};
};

struct CachedTextGeometry {
    std::vector<ID2D1Geometry*> parts;
    D2D1_RECT_F bounds{};
    bool has_bounds{};
};

struct PreviewRenderer {
    ID2D1Factory* d2d{};
    IDWriteFactory* dwrite{};
    IWICImagingFactory* wic{};
    ID2D1StrokeStyle* round_stroke_style{};
    ID2D1RenderTarget* target{};
    ID2D1HwndRenderTarget* hwnd_target{};
    ID2D1Layer* role_mask_layer{};
    ID2D1Layer* fade_opacity_layer{};
    std::unordered_map<std::uint32_t, ID2D1SolidColorBrush*> solid_brush_cache;
    std::unordered_map<std::string, IDWriteTextLayout*> text_layout_cache;
    std::unordered_map<std::string, CachedTextGeometry> text_geometry_cache;
    std::unordered_map<std::wstring, ID2D1Bitmap*> role_bitmap_cache;
    std::unordered_map<std::string, ID2D1Bitmap*> role_bitmap_data_cache;
    std::wstring font_family{L"MotoyaLMaru W3 Mono"};
    std::vector<std::wstring> font_families{font_family};

    struct CssLineMetrics {
        float baseline{};
        float height{};
    };
    std::unordered_map<std::string, CssLineMetrics> css_line_metrics_cache;
    std::unordered_map<std::string, float> dwrite_baseline_cache;

    // Per-frame gradient resource cache: avoids re-creating gradient stops
    // and brushes for every character.  Cleared at the start of each
    // render_attached() call (begin_frame / end_frame).
    struct GradientKey {
        float top, bottom, fade_in, fade_out;
        int role_idx, role_cnt;
        bool operator==(const GradientKey& o) const noexcept {
            return top == o.top && bottom == o.bottom
                && fade_in == o.fade_in && fade_out == o.fade_out
                && role_idx == o.role_idx && role_cnt == o.role_cnt;
        }
    };
    struct GradientKeyHash {
        std::size_t operator()(const GradientKey& k) const noexcept {
            auto h = std::hash<float>{}(k.top);
            h ^= std::hash<float>{}(k.bottom) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<float>{}(k.fade_in) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<float>{}(k.fade_out) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>{}(k.role_idx) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>{}(k.role_cnt) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
    struct CachedGradient {
        ID2D1GradientStopCollection* stops{};
        ID2D1LinearGradientBrush* brush{};
    };
    std::unordered_map<GradientKey, CachedGradient, GradientKeyHash> grad_cache;

    // --- Glyph bitmap cache ---
    // Pre-renders geometry to white masks. Per-frame: FillOpacityMask.
    // Colour-independent: N-role just swaps the brush.
    struct CachedGlyphBmp {
        ID2D1Bitmap* fill{};
        ID2D1Bitmap* stroke{};
        float ox{}, oy{};
        D2D1_SIZE_F sz{};
        D2D1_RECT_F reveal{};
    };
    std::unordered_map<std::string, CachedGlyphBmp> glyph_bmp_cache;
    static constexpr float glyph_bitmap_supersample = 3.0F;

    void clear_glyph_bmp_cache() {
        for (auto& [k, v] : glyph_bmp_cache) { release(v.fill); release(v.stroke); }
        glyph_bmp_cache.clear();
    }

    const CachedGlyphBmp* get_glyph_bmp(std::string_view text, float fs,
        bool bold, float sw) {
        auto key = text_cache_key(text, fs, bold);
        key += '|' + std::to_string(static_cast<int>(sw * 10.0F));
        key += "|ss" + std::to_string(static_cast<int>(glyph_bitmap_supersample));
        if (auto it = glyph_bmp_cache.find(key); it != glyph_bmp_cache.end())
            return &it->second;

        const auto* geom = text_geometry(text, fs, bold);
        if (!geom || geom->parts.empty()) return nullptr;

        const float pad = sw * 2.0F + 4.0F;
        const float left = std::floor(geom->bounds.left - pad * 0.5F);
        const float top = std::floor(geom->bounds.top - pad * 0.5F);
        const float right = std::ceil(geom->bounds.right + pad * 0.5F);
        const float bottom = std::ceil(geom->bounds.bottom + pad * 0.5F);
        const float bw = std::max(1.0F, right - left);
        const float bh = std::max(1.0F, bottom - top);
        D2D1_SIZE_F bsz{bw, bh};
        const float ox = left;
        const float oy = top;
        const float reveal_pad = sw > 0.0F ? sw + 1.0F : 1.0F;
        const auto reveal = D2D1::RectF(
            geom->bounds.left - reveal_pad,
            geom->bounds.top - reveal_pad,
            geom->bounds.right + reveal_pad,
            geom->bounds.bottom + reveal_pad);
        const auto pixel_size = D2D1::SizeU(
            static_cast<UINT32>(std::max(1.0F,
                std::round(bw * glyph_bitmap_supersample))),
            static_cast<UINT32>(std::max(1.0F,
                std::round(bh * glyph_bitmap_supersample))));

        auto make_mask = [&](bool stroke_only) -> ID2D1Bitmap* {
            ID2D1BitmapRenderTarget* brt{};
            auto pf = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED);
            if (!target || FAILED(target->CreateCompatibleRenderTarget(
                    &bsz, &pixel_size, &pf,
                    D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &brt)))
                return nullptr;
            brt->BeginDraw();
            brt->Clear(D2D1::ColorF(0, 0, 0, 0));
            brt->SetTransform(D2D1::Matrix3x2F::Translation(-left, -top));
            ID2D1SolidColorBrush* wb{};
            brt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &wb);
            if (wb) {
                for (auto* g : geom->parts) {
                    if (sw > 0.0F && stroke_only)
                        brt->DrawGeometry(g, wb, native_text_stroke_width(sw), round_stroke_style);
                    if (!stroke_only)
                        brt->FillGeometry(g, wb);
                }
                wb->Release();
            }
            const auto result = brt->EndDraw();
            if (FAILED(result)) {
                brt->Release();
                return nullptr;
            }
            ID2D1Bitmap* bmp{};
            brt->GetBitmap(&bmp);
            brt->Release();
            return bmp;
        };

        auto* sb = make_mask(true);
        auto* fb = make_mask(false);
        if (!sb || !fb) { release(sb); release(fb); return nullptr; }
        CachedGlyphBmp c{fb, sb, ox, oy, bsz, reveal};
        auto [it, ok] = glyph_bmp_cache.emplace(std::move(key), c);
        return &it->second;
    }

    // Token-level bitmap for lower ruby: renders the whole token (multiple
    // characters with manual letter-spacing) into ONE bitmap so the stroke is
    // continuous across the token, matching the token-level walk clip.
    const CachedGlyphBmp* get_token_bmp(
        const std::vector<std::string>& chars,
        const std::vector<float>& widths, float ls, float fs, bool bold, float sw) {
        if (chars.size() == 1)
            return get_glyph_bmp(chars[0], fs, bold, sw);

        std::string key = "tok|";
        for (const auto& ch : chars) { key += std::string(ch); key += '|'; }
        for (const float w : widths) {
            key += std::to_string(static_cast<int>(std::round(w * 100.0F)));
            key += ',';
        }
        key += std::to_string(static_cast<int>(std::round(ls * 100.0F)));
        key += '|' + std::to_string(static_cast<int>(std::round(fs * 100.0F)));
        key += bold ? "|b" : "|n";
        key += '|' + std::to_string(static_cast<int>(sw * 10.0F));
        key += "|ss" + std::to_string(static_cast<int>(glyph_bitmap_supersample));
        if (auto it = glyph_bmp_cache.find(key); it != glyph_bmp_cache.end())
            return &it->second;

        // Per-character geometries positioned with manual letter-spacing.
        std::vector<const CachedTextGeometry*> geoms;
        std::vector<float> offsets;
        geoms.reserve(chars.size());
        offsets.reserve(chars.size());
        float dx = 0.0F;
        float left = std::numeric_limits<float>::max();
        float top = std::numeric_limits<float>::max();
        float right = -std::numeric_limits<float>::max();
        float bottom = -std::numeric_limits<float>::max();
        for (std::size_t i = 0; i < chars.size(); ++i) {
            const auto* geom = text_geometry(chars[i], fs, bold);
            if (!geom || geom->parts.empty()) return nullptr;
            const auto& b = geom->bounds;
            geoms.push_back(geom);
            offsets.push_back(dx);
            left = std::min(left, dx + b.left);
            top = std::min(top, b.top);
            right = std::max(right, dx + b.right);
            bottom = std::max(bottom, b.bottom);
            dx += widths[i];
            if (i + 1 < chars.size()) dx += ls;
        }
        if (!(right > left && bottom > top)) return nullptr;

        const float pad = sw * 2.0F + 4.0F;
        const float bl = std::floor(left - pad * 0.5F);
        const float bt = std::floor(top - pad * 0.5F);
        const float br = std::ceil(right + pad * 0.5F);
        const float bb = std::ceil(bottom + pad * 0.5F);
        const float bw = std::max(1.0F, br - bl);
        const float bh = std::max(1.0F, bb - bt);
        const D2D1_SIZE_F bsz{bw, bh};
        const float ox = bl, oy = bt;
        const float reveal_pad = sw > 0.0F ? sw + 1.0F : 1.0F;
        const auto reveal = D2D1::RectF(
            left - reveal_pad, top - reveal_pad,
            right + reveal_pad, bottom + reveal_pad);
        const auto pixel_size = D2D1::SizeU(
            static_cast<UINT32>(std::max(1.0F,
                std::round(bw * glyph_bitmap_supersample))),
            static_cast<UINT32>(std::max(1.0F,
                std::round(bh * glyph_bitmap_supersample))));

        auto make_mask = [&](bool stroke_only) -> ID2D1Bitmap* {
            ID2D1BitmapRenderTarget* brt{};
            auto pf = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED);
            if (!target || FAILED(target->CreateCompatibleRenderTarget(
                    &bsz, &pixel_size, &pf,
                    D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &brt)))
                return nullptr;
            brt->BeginDraw();
            brt->Clear(D2D1::ColorF(0, 0, 0, 0));
            ID2D1SolidColorBrush* wb{};
            brt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &wb);
            if (wb) {
                for (std::size_t i = 0; i < geoms.size(); ++i) {
                    brt->SetTransform(D2D1::Matrix3x2F::Translation(
                        offsets[i] - bl, -bt));
                    for (auto* g : geoms[i]->parts) {
                        if (sw > 0.0F && stroke_only)
                            brt->DrawGeometry(g, wb,
                                native_text_stroke_width(sw), round_stroke_style);
                        if (!stroke_only)
                            brt->FillGeometry(g, wb);
                    }
                }
                wb->Release();
            }
            const auto result = brt->EndDraw();
            if (FAILED(result)) { brt->Release(); return nullptr; }
            ID2D1Bitmap* bmp{};
            brt->GetBitmap(&bmp);
            brt->Release();
            return bmp;
        };

        auto* sb = make_mask(true);
        auto* fb = make_mask(false);
        if (!sb || !fb) { release(sb); release(fb); return nullptr; }
        CachedGlyphBmp c{fb, sb, ox, oy, bsz, reveal};
        auto [it, ok] = glyph_bmp_cache.emplace(std::move(key), c);
        return &it->second;
    }

    void clear_gradient_cache() {
        for (auto& [k, v] : grad_cache) {
            release(v.brush);
            release(v.stops);
        }
        grad_cache.clear();
    }

    CachedGradient* get_gradient(const GradientKey& key) {
        auto it = grad_cache.find(key);
        if (it != grad_cache.end()) return &it->second;
        if (!target) return nullptr;

        std::array<D2D1_GRADIENT_STOP, 4> stops_arr{
            D2D1::GradientStop(0.0F, D2D1::ColorF(0, key.role_idx == 0 ? 1.0F : 0.0F)),
            D2D1::GradientStop(key.fade_in, D2D1::ColorF(0, 1.0F)),
            D2D1::GradientStop(key.fade_out, D2D1::ColorF(0, 1.0F)),
            D2D1::GradientStop(1.0F, D2D1::ColorF(0,
                key.role_idx + 1 == key.role_cnt ? 1.0F : 0.0F)),
        };

        CachedGradient g;
        target->CreateGradientStopCollection(stops_arr.data(),
            static_cast<UINT32>(stops_arr.size()), &g.stops);
        if (g.stops) {
            target->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(0.0F, key.top),
                    D2D1::Point2F(0.0F, key.bottom)),
                g.stops, &g.brush);
        }
        grad_cache[key] = g;
        return &grad_cache[key];
    }

    ~PreviewRenderer() {
        shutdown();
    }

    void shutdown() {
        clear_geometry_cache();
        clear_text_cache();
        clear_role_bitmap_cache();
        clear_brush_cache();
        clear_gradient_cache();
        clear_glyph_bmp_cache();
        release(role_mask_layer);
        release(fade_opacity_layer);
        release(target);
        release(round_stroke_style);
        release(wic);
        release(dwrite);
        release(d2d);
    }

    bool initialize() {
        const auto result = SUCCEEDED(D2D1CreateFactory(
                D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d))
            && SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dwrite)))
            && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
        if (result) create_round_stroke_style();
        return result;
    }

    void create_round_stroke_style() {
        release(round_stroke_style);
        if (!d2d) return;
        auto properties = D2D1::StrokeStyleProperties();
        properties.startCap = D2D1_CAP_STYLE_ROUND;
        properties.endCap = D2D1_CAP_STYLE_ROUND;
        properties.dashCap = D2D1_CAP_STYLE_ROUND;
        properties.lineJoin = D2D1_LINE_JOIN_ROUND;
        d2d->CreateStrokeStyle(properties, nullptr, 0, &round_stroke_style);
    }

    void clear_text_cache() {
        for (auto& [key, layout] : text_layout_cache) {
            static_cast<void>(key);
            release(layout);
        }
        text_layout_cache.clear();
        css_line_metrics_cache.clear();
        dwrite_baseline_cache.clear();
    }

    void clear_geometry_cache() {
        for (auto& [key, cached] : text_geometry_cache) {
            static_cast<void>(key);
            for (auto*& geometry : cached.parts) release(geometry);
        }
        text_geometry_cache.clear();
    }

    void set_font_family(std::wstring value) {
        if (value.empty() || value == font_family) return;
        font_family = std::move(value);
        font_families = {font_family};
        clear_geometry_cache();
        clear_text_cache();
        clear_glyph_bmp_cache();
    }

    void set_font_families(std::vector<std::wstring> values) {
        values.erase(std::remove_if(values.begin(), values.end(),
            [](const auto& value) { return value.empty(); }), values.end());
        if (values.empty()) values.push_back(L"MotoyaLMaru W3 Mono");
        if (values == font_families) return;
        font_families = std::move(values);
        font_family = font_families.front();
        clear_geometry_cache();
        clear_text_cache();
        clear_glyph_bmp_cache();
    }

    void discard_target() {
        clear_role_bitmap_cache();
        clear_brush_cache();
        clear_glyph_bmp_cache();
        release(role_mask_layer);
        release(fade_opacity_layer);
        hwnd_target = nullptr;
        release(target);
    }

    ID2D1Layer* reusable_role_mask_layer() {
        if (!target) return nullptr;
        if (!role_mask_layer) {
            target->CreateLayer(&role_mask_layer);
        }
        return role_mask_layer;
    }

    ID2D1Layer* reusable_fade_opacity_layer() {
        if (!target) return nullptr;
        if (!fade_opacity_layer) {
            target->CreateLayer(&fade_opacity_layer);
        }
        return fade_opacity_layer;
    }

    void clear_role_bitmap_cache() {
        for (auto& [path, bitmap] : role_bitmap_cache) {
            static_cast<void>(path);
            release(bitmap);
        }
        role_bitmap_cache.clear();
        for (auto& [key, bitmap] : role_bitmap_data_cache) {
            static_cast<void>(key);
            release(bitmap);
        }
        role_bitmap_data_cache.clear();
    }

    void clear_brush_cache() {
        for (auto& [key, cached_brush] : solid_brush_cache) {
            static_cast<void>(key);
            release(cached_brush);
        }
        solid_brush_cache.clear();
    }

    ID2D1Bitmap* role_bitmap(const std::wstring& path) {
        if (path.empty() || !target || !wic) return nullptr;
        if (const auto found = role_bitmap_cache.find(path);
                found != role_bitmap_cache.end()) return found->second;
        IWICBitmapDecoder* decoder{};
        IWICBitmapFrameDecode* frame{};
        IWICFormatConverter* converter{};
        ID2D1Bitmap* bitmap{};
        if (SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr,
                GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder))
                && SUCCEEDED(decoder->GetFrame(0, &frame))
                && SUCCEEDED(wic->CreateFormatConverter(&converter))
                && SUCCEEDED(converter->Initialize(frame,
                    GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                    nullptr, 0.0, WICBitmapPaletteTypeMedianCut))) {
            if (FAILED(target->CreateBitmapFromWicBitmap(
                    converter, nullptr, &bitmap))) {
                bitmap = nullptr;
            }
        }
        release(converter);
        release(frame);
        release(decoder);
        if (!bitmap) return nullptr;
        role_bitmap_cache.emplace(path, bitmap);
        return bitmap;
    }

    ID2D1Bitmap* role_bitmap_from_data(const std::string& role,
            const std::vector<std::uint8_t>& data) {
        if (data.empty() || !target || !wic) return nullptr;
        if (const auto found = role_bitmap_data_cache.find(role);
                found != role_bitmap_data_cache.end()) return found->second;
        IWICStream* stream{};
        IWICBitmapDecoder* decoder{};
        IWICBitmapFrameDecode* frame{};
        IWICFormatConverter* converter{};
        ID2D1Bitmap* bitmap{};
        if (SUCCEEDED(wic->CreateStream(&stream))
                && SUCCEEDED(stream->InitializeFromMemory(
                    const_cast<BYTE*>(data.data()),
                    static_cast<DWORD>(data.size())))
                && SUCCEEDED(wic->CreateDecoderFromStream(
                    stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder))
                && SUCCEEDED(decoder->GetFrame(0, &frame))
                && SUCCEEDED(wic->CreateFormatConverter(&converter))
                && SUCCEEDED(converter->Initialize(frame,
                    GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                    nullptr, 0.0, WICBitmapPaletteTypeMedianCut))) {
            if (FAILED(target->CreateBitmapFromWicBitmap(
                    converter, nullptr, &bitmap))) {
                bitmap = nullptr;
            }
        }
        release(converter);
        release(frame);
        release(decoder);
        release(stream);
        if (!bitmap) return nullptr;
        role_bitmap_data_cache.emplace(role, bitmap);
        return bitmap;
    }

    ID2D1SolidColorBrush* brush(D2D1_COLOR_F color) {
        if (!target) return nullptr;
        const auto quantize = [](float value) {
            return static_cast<std::uint32_t>(
                std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
        };
        const auto key = quantize(color.r)
            | (quantize(color.g) << 8U)
            | (quantize(color.b) << 16U)
            | (quantize(color.a) << 24U);
        if (const auto found = solid_brush_cache.find(key);
                found != solid_brush_cache.end()) {
            return found->second;
        }
        ID2D1SolidColorBrush* created{};
        if (FAILED(target->CreateSolidColorBrush(color, &created))) return nullptr;
        solid_brush_cache.emplace(key, created);
        return created;
    }

    bool ensure_target(HWND hwnd) {
        if (target) return true;
        RECT rect{};
        GetClientRect(hwnd, &rect);
        const auto size = D2D1::SizeU(
            static_cast<UINT32>(std::max(1L, rect.right - rect.left)),
            static_cast<UINT32>(std::max(1L, rect.bottom - rect.top)));
        ID2D1HwndRenderTarget* created{};
        const auto result = SUCCEEDED(d2d->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd, size), &created));
        if (result) {
            target = created;
            hwnd_target = created;
        }
        return result;
    }

    bool attach_external_target(ID2D1RenderTarget* external) {
        if (!external) return false;
        if (external == target) return true;
        if (same_d2d_resource_domain(target, external)) {
            external->AddRef();
            release(target);
            target = external;
            hwnd_target = nullptr;
            return true;
        }
        clear_geometry_cache();
        discard_target();
        external->AddRef();
        target = external;
        ID2D1Factory* external_factory{};
        external->GetFactory(&external_factory);
        if (external_factory) {
            release(round_stroke_style);
            release(d2d);
            d2d = external_factory;
            create_round_stroke_style();
        }
        return true;
    }

    void resize_hwnd_target(UINT width, UINT height) {
        if (hwnd_target) {
            hwnd_target->Resize(D2D1::SizeU(std::max(1U, width),
                std::max(1U, height)));
        }
    }

    static std::wstring lower_ascii(std::wstring value) {
        std::transform(value.begin(), value.end(), value.begin(),
            [](wchar_t ch) {
                return ch >= L'A' && ch <= L'Z'
                    ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
            });
        return value;
    }

    static void append_unique(std::vector<std::wstring>& target,
        std::wstring value) {
        if (value.empty()) return;
        if (std::find(target.begin(), target.end(), value) == target.end()) {
            target.push_back(std::move(value));
        }
    }

    static std::vector<std::wstring> expanded_font_families(
        const std::vector<std::wstring>& requested) {
        std::vector<std::wstring> result;
        for (const auto& family : requested) {
            const auto generic = lower_ascii(family);
            if (generic == L"sans-serif" || generic == L"system-ui") {
                append_unique(result, L"Microsoft YaHei UI");
                append_unique(result, L"Yu Gothic UI");
                append_unique(result, L"Meiryo");
                append_unique(result, L"Segoe UI");
            } else if (generic == L"serif") {
                append_unique(result, L"Yu Mincho");
                append_unique(result, L"MS Mincho");
                append_unique(result, L"SimSun");
                append_unique(result, L"Times New Roman");
            } else if (generic == L"monospace") {
                append_unique(result, L"Cascadia Mono");
                append_unique(result, L"Consolas");
                append_unique(result, L"MS Gothic");
                append_unique(result, L"Courier New");
            } else {
                append_unique(result, family);
            }
        }
        append_unique(result, L"Microsoft YaHei UI");
        append_unique(result, L"Yu Gothic UI");
        append_unique(result, L"Meiryo");
        append_unique(result, L"Segoe UI");
        append_unique(result, L"Arial");
        return result;
    }

    IDWriteTextFormat* format(float size, bool bold) {
        IDWriteTextFormat* result{};
        const auto candidates = expanded_font_families(font_families);
        for (const auto& family : candidates) {
            if (SUCCEEDED(dwrite->CreateTextFormat(family.c_str(), nullptr,
                    bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    size, L"zh-CN", &result)) && result) {
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

    float measure(std::string_view text, float size, bool bold) {
        auto* layout = text_layout(text, size, bold);
        if (!layout) return size;
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        release(layout);
        return std::max(1.0F, metrics.widthIncludingTrailingWhitespace);
    }

    float measure_track(const RubyTrack& track, const DemoStyle& style,
        bool lower = false) {
        if (track.text.empty()) return 0.0F;
        const auto font_size = native_ruby_font_size(
            lower ? style.ruby2_size : style.ruby_size);
        const auto letter_spacing = lower
            ? style.ruby2_letter_spacing : style.ruby_letter_spacing;
        const auto bold = lower ? style.ruby2_bold : false;
        float width = 0.0F;
        for (const auto& character : utf8_chars(track.text)) {
            width += measure(character, font_size, bold);
        }
        const auto count = utf8_chars(track.text).size();
        if (count > 1) width += static_cast<float>(count - 1)
            * letter_spacing;
        return width;
    }

    static std::vector<std::string> utf8_chars(std::string_view text) {
        std::vector<std::string> result;
        for (std::size_t i = 0; i < text.size();) {
            const auto lead = static_cast<unsigned char>(text[i]);
            const std::size_t count = (lead & 0x80U) == 0 ? 1
                : (lead & 0xE0U) == 0xC0U ? 2
                : (lead & 0xF0U) == 0xE0U ? 3 : 4;
            const auto safe = std::min(count, text.size() - i);
            result.emplace_back(text.substr(i, safe));
            i += safe;
        }
        return result;
    }

    static std::string text_cache_key(std::string_view text, float size, bool bold) {
        auto key = std::string{text};
        key += '|';
        key += std::to_string(static_cast<int>(std::round(size * 100.0F)));
        key += bold ? "|b" : "|n";
        return key;
    }

    IDWriteTextLayout* text_layout(std::string_view text, float size, bool bold) {
        auto key = text_cache_key(text, size, bold);
        if (const auto found = text_layout_cache.find(key);
                found != text_layout_cache.end()) {
            found->second->AddRef();
            return found->second;
        }
        const auto wide = utf8_to_wide(text);
        auto* text_format = format(size, bold);
        IDWriteTextLayout* layout{};
        if (text_format) {
            dwrite->CreateTextLayout(wide.c_str(), static_cast<UINT32>(wide.size()),
                text_format, 4096.0F, size * 1.5F, &layout);
        }
        release(text_format);
        if (layout) {
            layout->AddRef();
            text_layout_cache.emplace(std::move(key), layout);
        }
        return layout;
    }

    CssLineMetrics natural_line_metrics(float size, bool bold) {
        auto key = text_cache_key("M", size, bold);
        key += "|natural-line";
        if (const auto found = css_line_metrics_cache.find(key);
                found != css_line_metrics_cache.end()) {
            return found->second;
        }

        CssLineMetrics result{size * 0.88F, size};
        auto* layout = text_layout("M", size, bold);
        if (layout) {
            DWRITE_LINE_METRICS metrics{};
            UINT32 actual_count{};
            if (SUCCEEDED(layout->GetLineMetrics(&metrics, 1, &actual_count))
                    && actual_count > 0 && metrics.height > 0.0F
                    && metrics.baseline > 0.0F) {
                result = {metrics.baseline, metrics.height};
            }
            release(layout);
        }
        css_line_metrics_cache.emplace(std::move(key), result);
        return result;
    }

    CssLineMetrics css_line_metrics(float size, bool bold, float line_height) {
        auto key = text_cache_key("M", size, bold);
        key += "|css-line|" + std::to_string(static_cast<int>(
            std::round(line_height * 100.0F)));
        if (const auto found = css_line_metrics_cache.find(key);
                found != css_line_metrics_cache.end()) {
            return found->second;
        }

        const auto natural = natural_line_metrics(size, bold);
        const auto css_height = size * line_height;
        const auto half_leading = (css_height - natural.height) * 0.5F;
        CssLineMetrics result{natural.baseline + half_leading, css_height};
        css_line_metrics_cache.emplace(std::move(key), result);
        return result;
    }

    float dwrite_baseline(float size, bool bold) {
        auto key = text_cache_key("M", size, bold);
        key += "|dwrite-baseline";
        if (const auto found = dwrite_baseline_cache.find(key);
                found != dwrite_baseline_cache.end()) {
            return found->second;
        }
        const auto baseline = natural_line_metrics(size, bold).baseline;
        dwrite_baseline_cache.emplace(std::move(key), baseline);
        return baseline;
    }

    float layout_top_from_css_line_top(float top, float size, bool bold,
        float line_height) {
        return top + css_line_metrics(size, bold, line_height).baseline
            - dwrite_baseline(size, bold);
    }

    const CachedTextGeometry* text_geometry(std::string_view text, float size,
        bool bold) {
        auto key = text_cache_key(text, size, bold);
        if (const auto found = text_geometry_cache.find(key);
                found != text_geometry_cache.end()) return &found->second;
        auto* layout = text_layout(text, size, bold);
        if (!layout) return nullptr;
        auto* collector = new GeometryCollector(d2d);
        const auto result = layout->Draw(nullptr, collector, 0.0F, 0.0F);
        release(layout);
        CachedTextGeometry cached;
        if (SUCCEEDED(result)) {
            cached.parts = collector->take_geometries();
            cached.bounds = collector->bounds();
            cached.has_bounds = collector->has_bounds();
        }
        collector->Release();
        if (FAILED(result) || cached.parts.empty()) {
            for (auto*& geometry : cached.parts) release(geometry);
            return nullptr;
        }
        return &text_geometry_cache.emplace(std::move(key),
            std::move(cached)).first->second;
    }

    D2D1_RECT_F draw_stroked(std::string_view text, float x, float y, float width,
        float size, bool bold, kirakara::show::PackedColor fill,
        kirakara::show::PackedColor stroke,
        float stroke_width, float opacity) {
        auto bounds = D2D1::RectF(x, y, x + width, y + size * 1.2F);
        const auto* bmp = get_glyph_bmp(text, size, bold, stroke_width);
        if (!bmp) return bounds;

        D2D1_RECT_F dr{x + bmp->ox, y + bmp->oy,
            x + bmp->ox + bmp->sz.width,
            y + bmp->oy + bmp->sz.height};
        D2D1_RECT_F reveal{x + bmp->reveal.left, y + bmp->reveal.top,
            x + bmp->reveal.right, y + bmp->reveal.bottom};

        const auto oldAA = target->GetAntialiasMode();
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        if (stroke_width > 0.0F && bmp->stroke) {
            if (auto* sb = brush(color_from_ref(stroke, opacity)))
                target->FillOpacityMask(bmp->stroke, sb,
                    D2D1_OPACITY_MASK_CONTENT_GRAPHICS, &dr, nullptr);
        }
        if (bmp->fill) {
            if (auto* fb = brush(color_from_ref(fill, opacity)))
                target->FillOpacityMask(bmp->fill, fb,
                    D2D1_OPACITY_MASK_CONTENT_GRAPHICS, &dr, nullptr);
        }
        target->SetAntialiasMode(oldAA);
        return reveal;
    }

    void draw_masked(std::string_view text, float x, float y, float width,
        float size, bool bold, double progress, const DemoStyle& style,
        float opacity, float stroke_scale = 1.0F,
        const TextPaint* paint_override = nullptr) {
        const auto paint = paint_override ? *paint_override : global_paint(style);
        const auto ink = draw_stroked(text, x, y, width, size, bold, paint.before,
            paint.stroke_before, style.stroke_width * stroke_scale, opacity);
        const auto clamped_progress = std::clamp(progress, 0.0, 1.0);
        const auto reveal = ink.left
            + static_cast<float>(clamped_progress) * std::max(0.0F, ink.right - ink.left);
        target->PushAxisAlignedClip(D2D1::RectF(
            ink.left, ink.top, reveal, ink.bottom),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        draw_stroked(text, x, y, width, size, bold, paint.after,
            paint.stroke_after,
            style.stroke_width * stroke_scale + 0.5F, opacity);
        target->PopAxisAlignedClip();
    }

    void draw_role_masked(std::string_view text, float x, float y, float width,
        float size, bool bold, double progress, const DemoStyle& style,
        float opacity, std::span<const std::string> roles,
        float stroke_scale = 1.0F, float role_box_top = std::numeric_limits<float>::quiet_NaN(),
        float role_box_height = 0.0F) {
        if (roles.size() < 2 || !style.role_colors) {
            const auto paint = roles.empty() ? global_paint(style)
                : role_paint(roles.front(), style);
            draw_masked(text, x, y, width, size, bold, progress, style,
                opacity, stroke_scale, &paint);
            return;
        }

        const float feather = 1.00F;
        if (!std::isfinite(role_box_top)) {
            role_box_top = y + dwrite_baseline(size, bold) - size * 0.88F;
        }
        if (role_box_height <= 0.0F) role_box_height = size;
        const float em_top = role_box_top;
        const float em_bottom = role_box_top + role_box_height;
        constexpr float role_split_offset = 0.04F;
        const auto bounds = D2D1::RectF(x - style.stroke_width - 4.0F,
            y - style.stroke_width - 4.0F,
            x + width + size + style.stroke_width + 4.0F,
            y + size * 1.5F + style.stroke_width + 4.0F);

        for (std::size_t index = 0; index < roles.size(); ++index) {
            const auto top = index == 0 ? bounds.top
                : em_top + (em_bottom - em_top) * (role_split_offset
                    + static_cast<float>(index)
                        / static_cast<float>(roles.size()));
            const auto bottom = index + 1 == roles.size() ? bounds.bottom
                : em_top + (em_bottom - em_top) * (role_split_offset
                    + static_cast<float>(index + 1)
                        / static_cast<float>(roles.size()));
            const auto gradient_top = index == 0 ? bounds.top : top - feather;
            const auto gradient_bottom = index + 1 == roles.size()
                ? bounds.bottom : bottom + feather;
            const auto range = std::max(1.0F, gradient_bottom - gradient_top);
            const auto fade_in = std::clamp((top - gradient_top) / range, 0.0F, 1.0F);
            const auto fade_out = std::clamp((bottom - gradient_top) / range, 0.0F, 1.0F);

            GradientKey gk{gradient_top, gradient_bottom, fade_in, fade_out,
                static_cast<int>(index), static_cast<int>(roles.size())};
            auto* cg = get_gradient(gk);
            auto* layer = reusable_role_mask_layer();
            if (cg && cg->brush && layer) {
                target->PushLayer(D2D1::LayerParameters(bounds, nullptr,
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                    D2D1::Matrix3x2F::Identity(), 1.0F, cg->brush), layer);
                const auto paint = role_paint(roles[index], style);
                draw_masked(text, x, y, width, size, bold, progress, style,
                    opacity, stroke_scale, &paint);
                target->PopLayer();
            }
        }
    }

    float group_base_width(const LyricLine& line, std::size_t first,
        std::size_t count, const DemoStyle& style) {
        float base = 0.0F;
        for (std::size_t i = 0; i < count; ++i) {
            base += measure(line.chars[first + i].text, style.font_size, false);
            if (i + 1 < count) base += style.letter_spacing;
        }
        return base;
    }

    float group_ruby_width(const LyricLine& line, std::size_t first,
        const DemoStyle& style) {
        const auto& character = line.chars[first];
        return std::max(measure_track(character.ruby_above, style, false),
            measure_track(character.ruby_below, style, true));
    }

    float group_width(const LyricLine& line, std::size_t first, std::size_t count,
        const DemoStyle& style) {
        const auto base = group_base_width(line, first, count, style);
        return style.ruby_isolate
            ? std::max(base, group_ruby_width(line, first, style)) : base;
    }

    float ruby_extra_gap(const LyricLine& line, std::size_t first,
        std::size_t count, std::size_t next_first, std::size_t next_count,
        const DemoStyle& style) {
        if (style.ruby_isolate) return 0.0F;
        const auto ruby1 = group_ruby_width(line, first, style);
        const auto ruby2 = group_ruby_width(line, next_first, style);
        if (ruby1 <= 0.0F || ruby2 <= 0.0F) return 0.0F;
        const auto overflow = ruby1 - group_base_width(line, first, count, style)
            + ruby2 - group_base_width(line, next_first, next_count, style);
        return std::max(0.0F, overflow / 2.0F
            + style.ruby_letter_spacing - style.letter_spacing);
    }

    static std::string role_key(std::span<const std::string> roles) {
        std::string result;
        for (const auto& role : roles) {
            if (!result.empty()) result += '+';
            result += role;
        }
        return result;
    }

    float role_label_width(std::span<const std::string> roles,
        const DemoStyle& style) {
        const auto& pfx = style.role_label_prefix;
        const auto& sep = style.role_label_separator;
        const auto& sfx = style.role_label_suffix;

        // Collect *visible* roles first (DOM visibleLabels). Prefix attaches
        // to the first visible label, suffix to the last visible label, and
        // separators only appear between visible labels. Using the raw role
        // indexes here broke both prefix/suffix (a hidden first/last role
        // suppressed them) and right-aligned line width (a hidden role
        // between two visible ones still added a separator width).
        struct Visible {
            const CharacterProfile* profile;
            const std::string* role_name;
            float size;
            float spacing;
        };
        std::vector<Visible> visible;
        for (const auto& role : roles) {
            const auto it = style.character_profiles.find(role);
            if (it == style.character_profiles.end()) continue;
            const auto* p = &it->second;
            if (!p->show_label) continue;
            visible.push_back(Visible{
                p,
                &role,
                std::round(style.font_size * p->label_scale / 100.0F),
                style.letter_spacing * p->label_scale / 100.0F,
            });
        }
        if (visible.empty()) return 0.0F;

        const auto measure_text = [&](std::string_view text, float size,
                                       float spacing) {
            float w = 0.0F;
            const auto chars = utf8_chars(text);
            for (std::size_t i = 0; i < chars.size(); ++i) {
                w += measure(chars[i], size, false);
                if (i + 1 < chars.size()) w += spacing;
            }
            return w;
        };

        float width = 0.0F;
        if (!pfx.empty()) {
            const auto& first = visible.front();
            width += measure_text(pfx, first.size, first.spacing)
                + style.letter_spacing + 2.0F;
        }
        for (std::size_t vi = 0; vi < visible.size(); ++vi) {
            const auto& v = visible[vi];
            const auto* p = v.profile;
            const auto label = p->display_name.empty()
                ? *v.role_name : p->display_name;
            const auto label_chars = utf8_chars(label);
            float item_width = 0.0F;
            bool is_image = false;
            if (p->image_mode && (!p->image_path.empty()
                    || !p->image_data.empty())) {
                auto* bmp = p->image_data.empty()
                    ? role_bitmap(p->image_path)
                    : role_bitmap_from_data(*v.role_name, p->image_data);
                if (bmp) {
                    const auto bs = bmp->GetSize();
                    if (bs.height > 0.0F) item_width = bs.width / bs.height * v.size;
                }
                item_width += p->label_margin_left + p->label_margin_right;
                is_image = true;
            } else {
                for (const auto& ch : label_chars)
                    item_width += measure(ch, v.size, false);
                if (label_chars.size() > 1)
                    item_width += static_cast<float>(label_chars.size() - 1)
                        * v.spacing;
            }
            const bool last_no_suffix = vi == visible.size() - 1 && sfx.empty();
            const float gap = is_image
                ? style.letter_spacing + 2.0F
                : (last_no_suffix ? style.letter_spacing
                                  : style.letter_spacing + 2.0F);
            width += item_width + gap;
            if (vi < visible.size() - 1 && !sep.empty()) {
                width += measure_text(sep, v.size, v.spacing)
                    + style.letter_spacing + 2.0F;
            }
        }
        if (!sfx.empty()) {
            const auto& last = visible.back();
            width += measure_text(sfx, last.size, last.spacing)
                + style.letter_spacing;
        }
        return width;
    }
    float line_width(const LyricLine& line, const DemoStyle& style) {
        float width = 0.0F;
        std::string last_role_key;
        for (std::size_t i = 0; i < line.chars.size();) {
            const auto span = std::min(std::max<std::size_t>(1, line.chars[i].ruby_span),
                line.chars.size() - i);
            const auto key = role_key(line.chars[i].roles);
            if (line.chars[i].role_explicit && !key.empty() && key != last_role_key) {
                width += role_label_width(line.chars[i].roles, style);
                last_role_key = key;
            }
            width += group_width(line, i, span, style);
            i += span;
            if (i < line.chars.size()) {
                const auto next_span = std::min(std::max<std::size_t>(1,
                    line.chars[i].ruby_span), line.chars.size() - i);
                width += style.letter_spacing + ruby_extra_gap(line,
                    i - span, span, i, next_span, style);
            }
        }
        return width;
    }

    float draw_role_labels(std::span<const std::string> roles, float cursor, float y,
        const DemoStyle& style, float opacity) {
        const auto& pfx = style.role_label_prefix;
        const auto& sep = style.role_label_separator;
        const auto& sfx = style.role_label_suffix;

        // Label colour helpers (matches DOM getRoleColor / getRoleStroke).
        auto label_fill = [&](const CharacterProfile* p) {
            if (p) {
                if (p->display_color) return p->display_color;
                if (p->color_before)  return p->color_before;
            }
            return style.before;
        };
        auto label_stroke_color = [&](const CharacterProfile* p) {
            if (p && p->label_stroke_color) return p->label_stroke_color;
            return style.stroke_before;
        };

        // Collect *visible* roles first (DOM visibleLabels). Prefix follows
        // the first visible label, suffix the last, separators only render
        // between visible labels. This keeps prefix/suffix working when the
        // first/last role is hidden, matching the web renderer.
        struct Visible {
            const CharacterProfile* profile;
            const std::string* role_name;
            float size;
            float spacing;
        };
        std::vector<Visible> visible;
        for (const auto& role : roles) {
            const auto it = style.character_profiles.find(role);
            if (it == style.character_profiles.end()) continue; // unknown -> no label
            const auto* p = &it->second;
            if (!p->show_label) continue;
            visible.push_back(Visible{
                p,
                &role,
                std::round(style.font_size * p->label_scale / 100.0F),
                style.letter_spacing * p->label_scale / 100.0F,
            });
        }
        if (visible.empty()) return cursor;

        // Draw helper: draw text with given profile's label colours.
        // DOM/Canvas scale the whole label block, so inter-character spacing
        // scales with labelScale while the trailing gap does not (DOM
        // marginRight sits outside the transform).
        auto draw_text = [&](std::string_view text, const CharacterProfile* p,
                              float size, float spacing, float trailing_gap) {
            const auto fill = label_fill(p);
            const auto stroke = label_stroke_color(p);
            const auto chars = utf8_chars(text);
            for (std::size_t i = 0; i < chars.size(); ++i) {
                const auto cw = measure(chars[i], size, false);
                draw_stroked(chars[i], cursor, y + style.font_size - size,
                    cw, size, style.font_bold, fill, stroke,
                    style.stroke_width, opacity);
                cursor += cw;
                if (i + 1 < chars.size()) cursor += spacing;
            }
            cursor += trailing_gap;
        };

        if (!pfx.empty()) {
            const auto& first = visible.front();
            draw_text(pfx, first.profile, first.size, first.spacing,
                style.letter_spacing + 2.0F);
        }

        for (std::size_t vi = 0; vi < visible.size(); ++vi) {
            const auto& v = visible[vi];
            const auto* p = v.profile;
            const auto size = v.size;
            const auto label_spacing = v.spacing;
            const auto label = p->display_name.empty()
                ? *v.role_name : p->display_name;
            const auto off_y = p->label_offset_y;

            const auto label_chars = utf8_chars(label);
            const auto fill = label_fill(p);
            const auto stroke = label_stroke_color(p);
            float width = 0.0F;
            for (const auto& ch : label_chars) width += measure(ch, size, false);
            if (label_chars.size() > 1) width += static_cast<float>(label_chars.size() - 1) * label_spacing;
            bool image_drawn = false;
            const bool has_image = !p->image_path.empty() || !p->image_data.empty();
            const bool is_image_label = p->image_mode && has_image;
            if (is_image_label) cursor += p->label_margin_left;
            if (p->image_mode && has_image) {
                auto* bitmap = p->image_data.empty()
                    ? role_bitmap(p->image_path)
                    : role_bitmap_from_data(*v.role_name, p->image_data);
                if (bitmap) {
                    const auto bs = bitmap->GetSize();
                    if (bs.height > 0.0F) {
                        width = bs.width / bs.height * size;
                        const auto top = y + style.font_size - size + off_y;
                        target->DrawBitmap(bitmap,
                            D2D1::RectF(cursor, top, cursor + width, top + size),
                            opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                        cursor += width;
                        cursor += style.letter_spacing + 2.0F;
                        image_drawn = true;
                    }
                }
            }
            if (!image_drawn) {
                for (std::size_t i = 0; i < label_chars.size(); ++i) {
                    const auto cw = measure(label_chars[i], size, false);
                    draw_stroked(label_chars[i], cursor,
                        y + style.font_size - size,
                        cw, size, style.font_bold, fill, stroke,
                        style.stroke_width, opacity);
                    cursor += cw;
                    if (i + 1 < label_chars.size()) cursor += label_spacing;
                }
                const bool last_no_suffix = vi == visible.size() - 1 && sfx.empty();
                cursor += last_no_suffix
                    ? style.letter_spacing
                    : style.letter_spacing + 2.0F;
            }
            if (is_image_label) cursor += p->label_margin_right;

            // Separator between visible labels, follows the current role
            // (DOM renders sep with the current visible profile).
            if (vi < visible.size() - 1 && !sep.empty()) {
                draw_text(sep, p, size, label_spacing,
                    style.letter_spacing + 2.0F);
            }
        }

        // Suffix (after the last *visible* role, follows its colours).
        // DOM uses trailingGap=ls (no +2) for suffix, unlike prefix/sep.
        if (!sfx.empty()) {
            const auto& last = visible.back();
            const auto* p = last.profile;
            const auto sfill = label_fill(p);
            const auto sstroke = label_stroke_color(p);
            const auto sfx_chars = utf8_chars(sfx);
            for (std::size_t i = 0; i < sfx_chars.size(); ++i) {
                const auto cw = measure(sfx_chars[i], last.size, false);
                draw_stroked(sfx_chars[i], cursor, y + style.font_size - last.size,
                    cw, last.size, style.font_bold, sfill, sstroke,
                    style.stroke_width, opacity);
                cursor += cw;
                if (i + 1 < sfx_chars.size()) cursor += last.spacing;
            }
            cursor += style.letter_spacing;
        }
        return cursor;
    }
    void draw_ruby_track(const RubyTrack& track, float x, float y, float group_width_value,
        Seconds group_start, Seconds group_end, Seconds time, const DemoStyle& style,
        float opacity, std::span<const std::string> roles, bool lower) {
        if (track.text.empty()) return;
        const auto font_size = native_ruby_font_size(
            lower ? style.ruby2_size : style.ruby_size);
        const auto letter_spacing = lower
            ? style.ruby2_letter_spacing : style.ruby_letter_spacing;
        const auto stroke_width = lower
            ? style.ruby2_stroke_width : style.ruby_stroke_width;
        const auto bold = lower ? style.ruby2_bold : style.ruby_bold;
        const auto stroke_scale = style.stroke_width > 0.0F
            ? stroke_width / style.stroke_width : 0.0F;
        const auto track_width = measure_track(track, style, lower);
        const auto role_box_top = y + dwrite_baseline(font_size, bold)
            - font_size * 0.88F;
        const auto role_box_height = font_size;

        // Token-level rendering for lower ruby (romaji): stroke is continuous
        // across the token, and walk-progress uses ink bounds of the full token.
        const auto draw_chunk_token = [&](std::string_view text, float start_x,
                                           double chunk_progress) -> float {
            const auto characters = utf8_chars(text);
            float total = 0.0F;
            std::vector<float> widths;
            widths.reserve(characters.size());
            for (const auto& ch : characters) {
                const auto w = measure(ch, font_size, bold);
                widths.push_back(w);
                total += w;
            }
            if (characters.size() > 1)
                total += static_cast<float>(characters.size() - 1) * letter_spacing;

            // Ink bounds come from the whole-token bitmap so the reveal edge
            // matches the single continuous stroke mask exactly.
            const auto* token_bmp = get_token_bmp(characters, widths,
                letter_spacing, font_size, bold,
                style.stroke_width * stroke_scale);
            const auto ink_left = token_bmp
                ? start_x + token_bmp->reveal.left : start_x;
            const auto ink_top = token_bmp
                ? y + token_bmp->reveal.top : y;
            const auto ink_right = token_bmp
                ? start_x + token_bmp->reveal.right : start_x + total;
            const auto ink_bottom = token_bmp
                ? y + token_bmp->reveal.bottom : y + font_size * 1.5F;

            const auto progress_f = static_cast<float>(std::clamp(chunk_progress, 0.0, 1.0));
            const auto reveal = ink_left
                + progress_f * std::max(0.0F, ink_right - ink_left);
            const auto clip_rect = D2D1::RectF(
                ink_left, ink_top, reveal, ink_bottom);

            // Helper: draw the whole token in one pass using its continuous
            // bitmap.  stroke_bias adds extra width to the "after" (played)
            // pass so its stroke covers subpixel colour fringing from the fill.
            auto draw_token = [&](kirakara::show::PackedColor fill,
                                  kirakara::show::PackedColor stroke_c,
                                  float stroke_bias = 0.0F) {
                const auto sw = style.stroke_width * stroke_scale + stroke_bias;
                const auto* bmp = get_token_bmp(characters, widths,
                    letter_spacing, font_size, bold, sw);
                if (!bmp) return;
                const D2D1_RECT_F dr{start_x + bmp->ox, y + bmp->oy,
                    start_x + bmp->ox + bmp->sz.width,
                    y + bmp->oy + bmp->sz.height};
                const auto oldAA = target->GetAntialiasMode();
                target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
                if (sw > 0.0F && bmp->stroke) {
                    if (auto* sb = brush(color_from_ref(stroke_c, opacity)))
                        target->FillOpacityMask(bmp->stroke, sb,
                            D2D1_OPACITY_MASK_CONTENT_GRAPHICS, &dr, nullptr);
                }
                if (bmp->fill) {
                    if (auto* fb = brush(color_from_ref(fill, opacity)))
                        target->FillOpacityMask(bmp->fill, fb,
                            D2D1_OPACITY_MASK_CONTENT_GRAPHICS, &dr, nullptr);
                }
                target->SetAntialiasMode(oldAA);
            };

            if (roles.size() < 2 || !style.role_colors) {
                const auto paint = roles.empty() ? global_paint(style)
                    : role_paint(roles.front(), style);
                draw_token(paint.before, paint.stroke_before);
                target->PushAxisAlignedClip(clip_rect,
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                draw_token(paint.after, paint.stroke_after, 0.5F);
                target->PopAxisAlignedClip();
                return total;
            }

            // Multi-role: gradient layers (same vertical split as
            // draw_role_masked, applied over the full token).
            const float feather = 1.0F;
            const float em_top = role_box_top;
            const float em_bottom = role_box_top + role_box_height;
            constexpr float role_split_offset = 0.04F;
            const auto bounds = D2D1::RectF(
                ink_left - style.stroke_width - 4.0F,
                ink_top - style.stroke_width - 4.0F,
                ink_right + style.stroke_width + 4.0F,
                ink_bottom + style.stroke_width + 4.0F);

            for (std::size_t index = 0; index < roles.size(); ++index) {
                const auto top = index == 0 ? bounds.top
                    : em_top + (em_bottom - em_top) * (role_split_offset
                        + static_cast<float>(index)
                            / static_cast<float>(roles.size()));
                const auto bottom = index + 1 == roles.size() ? bounds.bottom
                    : em_top + (em_bottom - em_top) * (role_split_offset
                        + static_cast<float>(index + 1)
                            / static_cast<float>(roles.size()));
                const auto gradient_top = index == 0 ? bounds.top : top - feather;
                const auto gradient_bottom = index + 1 == roles.size()
                    ? bounds.bottom : bottom + feather;
                const auto range = std::max(1.0F, gradient_bottom - gradient_top);
                const auto fade_in = std::clamp((top - gradient_top) / range, 0.0F, 1.0F);
                const auto fade_out = std::clamp((bottom - gradient_top) / range, 0.0F, 1.0F);

                GradientKey gk{gradient_top, gradient_bottom, fade_in, fade_out,
                    static_cast<int>(index), static_cast<int>(roles.size())};
                auto* cg = get_gradient(gk);
                auto* layer = reusable_role_mask_layer();
                if (cg && cg->brush && layer) {
                    target->PushLayer(D2D1::LayerParameters(bounds, nullptr,
                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                        D2D1::Matrix3x2F::Identity(), 1.0F, cg->brush), layer);
                    const auto paint = role_paint(roles[index], style);
                    draw_token(paint.before, paint.stroke_before);
                    target->PushAxisAlignedClip(clip_rect,
                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    draw_token(paint.after, paint.stroke_after, 0.5F);
                    target->PopAxisAlignedClip();
                    target->PopLayer();
                }
            }

            return total;
        };

        // Upper ruby is per-kana/per-character.  If one timing chunk contains
        // multiple UTF-8 characters (e.g. "きょ"), split that chunk's time
        // evenly across those characters.  Ruby2 intentionally keeps the
        // token-level path above.
        const auto draw_chunk_char = [&](std::string_view text, float start_x,
                                          double chunk_progress) -> float {
            const auto characters = utf8_chars(text);
            float total = 0.0F;
            std::vector<float> widths;
            widths.reserve(characters.size());
            for (const auto& character : characters) {
                const auto width = measure(character, font_size, false);
                widths.push_back(width);
                total += width;
            }
            if (characters.size() > 1) {
                total += static_cast<float>(characters.size() - 1)
                    * letter_spacing;
            }

            const auto progress = std::clamp(chunk_progress, 0.0, 1.0);
            const auto count = static_cast<double>(characters.size());
            float draw_x = start_x;
            for (std::size_t i = 0; i < characters.size(); ++i) {
                const auto local = std::clamp(progress * count
                    - static_cast<double>(i), 0.0, 1.0);
                draw_role_masked(characters[i], draw_x, y, widths[i],
                    font_size, bold, local, style, opacity, roles, stroke_scale,
                    role_box_top, role_box_height);
                draw_x += widths[i];
                if (i + 1 < characters.size()) draw_x += letter_spacing;
            }
            return total;
        };

        const auto draw_chunk = [&](std::string_view text, float start_x,
                                    double chunk_progress) -> float {
            return lower
                ? draw_chunk_token(text, start_x, chunk_progress)
                : draw_chunk_char(text, start_x, chunk_progress);
        };

        float cursor = x + (group_width_value - track_width) / 2.0F;
        if (track.timing.size() > 1) {
            for (std::size_t i = 0; i < track.timing.size(); ++i) {
                const auto& item = track.timing[i];
                const auto start = group_start + item.offset;
                const auto end = i + 1 < track.timing.size()
                    ? group_start + track.timing[i + 1].offset : group_end;
                const auto width = draw_chunk(item.text, cursor,
                    temporal_progress(time, start, end));
                cursor += width;
                if (i + 1 < track.timing.size()) {
                    cursor += letter_spacing;
                }
            }
        } else {
            draw_chunk(track.text, cursor,
                temporal_progress(time, group_start, group_end));
        }
    }

    void draw_line(const LyricLine& line, const LineState& state, std::size_t slot,
        Seconds time, const DemoStyle& style, const EngineConfig& config) {
        const auto total_width = line_width(line, style);
        float cursor = slot == 0 ? style.line1_x
            : 1280.0F - style.line2_right - total_width;
        constexpr float main_line_height = 1.2F;
        constexpr float ruby_line_height = 1.1F;
        const float css_y = slot == 0 ? style.line1_y
            : style.line2_y;
        const auto main_metrics = css_line_metrics(style.font_size,
            style.font_bold, main_line_height);
        const auto y = layout_top_from_css_line_top(css_y, style.font_size,
            style.font_bold, main_line_height);
        const auto upper_ruby_size = native_ruby_font_size(style.ruby_size);
        const auto lower_ruby_size = native_ruby_font_size(style.ruby2_size);
        const auto upper_ruby_y = layout_top_from_css_line_top(
            css_y - style.ruby_above_offset - upper_ruby_size * ruby_line_height,
            upper_ruby_size, style.ruby_bold, ruby_line_height);
        const auto lower_ruby_y = layout_top_from_css_line_top(
            css_y + main_metrics.height + style.ruby_below_offset,
            lower_ruby_size, style.ruby2_bold, ruby_line_height);
        const auto main_role_box_top = y + dwrite_baseline(style.font_size,
            style.font_bold) - style.font_size * 0.88F;
        const auto main_role_box_height = style.font_size;
        const auto line_opacity = std::clamp(
            static_cast<float>(state.opacity), 0.0F, 1.0F);
        if (line_opacity <= 0.0F) return;
        float opacity = line_opacity;
        auto* fade_layer = line_opacity < 0.999F
            ? reusable_fade_opacity_layer() : nullptr;
        if (fade_layer) {
            target->PushLayer(D2D1::LayerParameters(
                D2D1::RectF(0.0F, 0.0F, 1280.0F, 720.0F),
                nullptr,
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::Matrix3x2F::Identity(),
                line_opacity),
                fade_layer);
            opacity = 1.0F;
        }
        std::string last_role_key;

        if (state.indicator_visible) {
            const auto size = config.indicator.size;
            const auto radius = size / 2.0F;
            const auto base_x = cursor + config.indicator.offset_x;
            const auto base_y = css_y - (style.ruby_size + style.ruby_above_offset
                + config.indicator.offset_y);
            for (std::size_t i = 0; i < state.indicator_opacity.size(); ++i) {
                const auto alpha = static_cast<float>(state.indicator_opacity[i]) * opacity;
                const auto fill_color = config.indicator.fill;
                const auto stroke_color = config.indicator.stroke;
                const auto cx = base_x + radius + static_cast<float>(i)
                    * (size + config.indicator.spacing);
                const auto cy = base_y - size + radius;
                const auto drawn_radius = std::max(0.0F,
                    radius - config.indicator.stroke_width / 2.0F);
                if (auto* fill = brush(D2D1::ColorF(fill_color.r,
                        fill_color.g, fill_color.b, fill_color.a * alpha))) {
                    target->FillEllipse(D2D1::Ellipse(
                        D2D1::Point2F(cx, cy), drawn_radius, drawn_radius), fill);
                }
                if (config.indicator.stroke_width > 0.0F) {
                    if (auto* stroke = brush(D2D1::ColorF(stroke_color.r,
                            stroke_color.g, stroke_color.b,
                            stroke_color.a * alpha))) {
                        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy),
                            drawn_radius, drawn_radius), stroke,
                            config.indicator.stroke_width);
                    }
                }
            }
        }

        for (std::size_t i = 0; i < line.chars.size();) {
            const auto span = std::min(std::max<std::size_t>(1, line.chars[i].ruby_span),
                line.chars.size() - i);
            const auto width = group_width(line, i, span, style);
            const auto key = role_key(line.chars[i].roles);
            if (line.chars[i].role_explicit && !key.empty() && key != last_role_key) {
                cursor = draw_role_labels(line.chars[i].roles, cursor, y, style, opacity);
                last_role_key = key;
            }
            const auto base_width = [&] {
                float value = 0.0F;
                for (std::size_t j = 0; j < span; ++j) {
                    value += measure(line.chars[i + j].text, style.font_size,
                        false);
                    if (j + 1 < span) value += style.letter_spacing;
                }
                return value;
            }();
            float char_cursor = cursor + (width - base_width) / 2.0F;
            for (std::size_t j = 0; j < span; ++j) {
                const auto& character = line.chars[i + j];
                const auto char_width = measure(character.text, style.font_size,
                    false);
                const auto progress = i + j < state.chars.size()
                    ? state.chars[i + j].progress : 0.0;
                draw_role_masked(character.text, char_cursor, y, char_width,
                    style.font_size, style.font_bold, progress, style, opacity,
                    character.roles, 1.0F, main_role_box_top,
                    main_role_box_height);
                char_cursor += char_width;
                if (j + 1 < span) char_cursor += style.letter_spacing;
            }
            const auto& first = line.chars[i];
            const auto group_start = first.start_time;
            const auto group_end = line.chars[i + span - 1].end_time;
            draw_ruby_track(first.ruby_above, cursor, upper_ruby_y,
                width, group_start, group_end, time, style, opacity,
                first.roles, false);
            draw_ruby_track(first.ruby_below, cursor, lower_ruby_y,
                width, group_start, group_end, time, style, opacity,
                first.roles, true);
            const auto next = i + span;
            cursor += width;
            if (next < line.chars.size()) {
                const auto next_span = std::min(std::max<std::size_t>(1,
                    line.chars[next].ruby_span), line.chars.size() - next);
                cursor += style.letter_spacing + ruby_extra_gap(line,
                    i, span, next, next_span, style);
            }
            i = next;
        }
        if (fade_layer) {
            target->PopLayer();
        }
    }

    bool render_attached_frame(
        const PreparedDocument& document,
        const FrameState& frame,
        Seconds current_time,
        const EngineConfig& config,
        const DemoStyle& style,
        bool draw_background_layer) {
        if (!target) return false;
        clear_gradient_cache(); // fresh per frame
        const auto size = target->GetSize();
        const auto pixel_width = std::max(1.0F, size.width);
        const auto pixel_height = std::max(1.0F, size.height);
        const auto scale = std::min(pixel_width / 1280.0F, pixel_height / 720.0F);
        const auto offset_x = (pixel_width - 1280.0F * scale) / 2.0F;
        const auto offset_y = (pixel_height - 720.0F * scale) / 2.0F;
        D2D1_MATRIX_3X2_F previous_transform{};
        target->GetTransform(&previous_transform);

        target->BeginDraw();
        if (draw_background_layer) {
            target->Clear(color_from_ref(style.background, 1.0F));
        }
        target->SetTransform(D2D1::Matrix3x2F(scale, 0, 0, scale, offset_x, offset_y));

        for (std::size_t slot = 0; slot < 2; ++slot) {
            if (!frame.has_line[slot]) continue;
            const auto source = frame.slots[slot].source_line;
            if (source < document.lines.size()) {
                draw_line(document.lines[source], frame.slots[slot], slot,
                    current_time, style, config);
            }
        }

        // This target can be shared with the Stage compositor.  Do not leak
        // the 1280x720 lyric-space transform into the next video frame.
        target->SetTransform(previous_transform);
        const auto result = target->EndDraw();
        if (result == D2DERR_RECREATE_TARGET) discard_target();
        return SUCCEEDED(result);
    }

    bool render_attached(const PreparedDocument& document, Seconds current_time,
        const EngineConfig& config, const DemoStyle& style,
        bool draw_background_layer = true) {
        const auto frame = evaluate_frame(document, current_time, config);
        return render_attached_frame(document, frame, current_time,
            config, style, draw_background_layer);
    }

    bool render(HWND hwnd, const PreparedDocument& document, Seconds current_time,
        const EngineConfig& config, const DemoStyle& style) {
        return ensure_target(hwnd)
            && render_attached(document, current_time, config, style);
    }

    bool render_overlay(HWND hwnd, const PreparedDocument& document,
        Seconds current_time, const EngineConfig& config,
        const DemoStyle& style) {
        return ensure_target(hwnd)
            && render_attached(document, current_time, config, style, false);
    }
};


} // namespace

namespace kirakara::show::win32 {

struct LyricRenderer::Impl {
    Impl() : renderer(new PreviewRenderer) {}
    ~Impl() { delete static_cast<PreviewRenderer*>(renderer); }

    PreviewRenderer& get() noexcept {
        return *static_cast<PreviewRenderer*>(renderer);
    }
    const PreviewRenderer& get() const noexcept {
        return *static_cast<const PreviewRenderer*>(renderer);
    }

    void* renderer{};
};

LyricRenderer::LyricRenderer() : impl_(std::make_unique<Impl>()) {}
LyricRenderer::~LyricRenderer() = default;
LyricRenderer::LyricRenderer(LyricRenderer&&) noexcept = default;
LyricRenderer& LyricRenderer::operator=(LyricRenderer&&) noexcept = default;

bool LyricRenderer::initialize() {
    return impl_ && impl_->get().initialize();
}

void LyricRenderer::shutdown() {
    if (impl_) impl_->get().shutdown();
}

void LyricRenderer::set_font_family(std::wstring family) {
    if (impl_) impl_->get().set_font_family(std::move(family));
}

void LyricRenderer::set_font_families(std::vector<std::wstring> families) {
    if (impl_) impl_->get().set_font_families(std::move(families));
}

const std::wstring& LyricRenderer::font_family() const noexcept {
    static const std::wstring empty;
    return impl_ ? impl_->get().font_family : empty;
}

const std::vector<std::wstring>& LyricRenderer::font_families() const noexcept {
    static const std::vector<std::wstring> empty;
    return impl_ ? impl_->get().font_families : empty;
}

RendererBackendInfo LyricRenderer::backend_info() const noexcept {
    return {
        .backend = RendererBackend::direct2d,
        .name = "Direct2D",
        .hardware_accelerated = true,
    };
}

bool LyricRenderer::attach_target(const NativeRenderTarget& target) {
    if (!impl_ || target.kind != RenderTargetKind::direct2d_render_target
            || !target.handle) {
        return false;
    }
    return impl_->get().attach_external_target(
        static_cast<ID2D1RenderTarget*>(target.handle));
}

bool LyricRenderer::attach_native_target(void* render_target) {
    return attach_target(NativeRenderTarget{
        .kind = RenderTargetKind::direct2d_render_target,
        .handle = render_target,
    });
}

void LyricRenderer::resize_window_target(unsigned width, unsigned height) {
    if (impl_) impl_->get().resize_hwnd_target(width, height);
}

bool LyricRenderer::render(const PreparedDocument& document,
    Seconds current_time, const EngineConfig& config, const RenderStyle& style) {
    return impl_ && impl_->get().render_attached(
        document, current_time, config, style);
}

bool LyricRenderer::render_overlay(const PreparedDocument& document,
    Seconds current_time, const EngineConfig& config, const RenderStyle& style) {
    return impl_ && impl_->get().render_attached(
        document, current_time, config, style, false);
}

bool LyricRenderer::render_overlay_frame(
    const PreparedDocument& document,
    const FrameState& frame,
    Seconds current_time,
    const EngineConfig& config,
    const RenderStyle& style) {
    return impl_ && impl_->get().render_attached_frame(
        document, frame, current_time, config, style, false);
}

bool LyricRenderer::render_overlay_window(void* hwnd,
    const PreparedDocument& document, Seconds current_time,
    const EngineConfig& config, const RenderStyle& style) {
    return impl_ && impl_->get().render_overlay(static_cast<HWND>(hwnd),
        document, current_time, config, style);
}

bool LyricRenderer::render_window(void* hwnd,
    const PreparedDocument& document, Seconds current_time,
    const EngineConfig& config, const RenderStyle& style) {
    return impl_ && impl_->get().render(static_cast<HWND>(hwnd),
        document, current_time, config, style);
}

} // namespace kirakara::show::win32
