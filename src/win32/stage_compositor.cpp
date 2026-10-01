#include "kirakara/show/win32/stage_compositor.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi.h>

namespace kirakara::show::win32 {
namespace {

bool valid_canvas(const StageCanvas& canvas) {
    return canvas.width > 0 && canvas.height > 0
        && canvas.frame_rate_num > 0 && canvas.frame_rate_den > 0;
}

template <typename T>
void release(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

} // namespace

struct StageCompositor::SourceBitmapCache {
    struct Entry {
        ID3D11Texture2D* texture{};
        ID2D1Bitmap1* bitmap{};
        std::uint64_t last_used{};

        Entry() = default;
        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;
        Entry(Entry&& other) noexcept
            : texture(std::exchange(other.texture, nullptr)),
              bitmap(std::exchange(other.bitmap, nullptr)),
              last_used(other.last_used) {}
        Entry& operator=(Entry&& other) noexcept {
            if (this == &other) return *this;
            release(bitmap);
            release(texture);
            texture = std::exchange(other.texture, nullptr);
            bitmap = std::exchange(other.bitmap, nullptr);
            last_used = other.last_used;
            return *this;
        }
        ~Entry() {
            release(bitmap);
            release(texture);
        }
    };

    static constexpr std::size_t capacity = 8;
    std::vector<Entry> entries;
    std::uint64_t use_counter{};

    void clear() {
        entries.clear();
        use_counter = 0;
    }

    ID2D1Bitmap1* bitmap_for(
            ID2D1DeviceContext* target, ID3D11Texture2D* texture) {
        if (!target || !texture) return nullptr;
        ++use_counter;
        for (auto& entry : entries) {
            if (entry.texture == texture) {
                entry.last_used = use_counter;
                return entry.bitmap;
            }
        }

        IDXGISurface* surface{};
        auto result = texture->QueryInterface(IID_PPV_ARGS(&surface));
        ID2D1Bitmap1* bitmap{};
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0F,
            96.0F);
        if (SUCCEEDED(result)) {
            result = target->CreateBitmapFromDxgiSurface(
                surface, &properties, &bitmap);
        }
        release(surface);
        if (FAILED(result) || !bitmap) {
            release(bitmap);
            return nullptr;
        }

        if (entries.size() >= capacity) {
            const auto oldest = std::min_element(
                entries.begin(), entries.end(), [](const auto& left,
                        const auto& right) {
                    return left.last_used < right.last_used;
                });
            entries.erase(oldest);
        }
        texture->AddRef();
        entries.emplace_back();
        auto& entry = entries.back();
        entry.texture = texture;
        entry.bitmap = bitmap;
        entry.last_used = use_counter;
        return entry.bitmap;
    }
};

StageCompositor::StageCompositor()
    : source_bitmap_cache_(std::make_unique<SourceBitmapCache>()) {}
StageCompositor::~StageCompositor() = default;
StageCompositor::StageCompositor(StageCompositor&& other) noexcept
    : canvas_(other.canvas_),
      video_rect_(other.video_rect_),
      overlay_(std::move(other.overlay_)),
      active_surface_(&overlay_),
      source_bitmap_cache_(std::move(other.source_bitmap_cache_)),
      defer_d2d_interop_flush_(other.defer_d2d_interop_flush_) {}
StageCompositor& StageCompositor::operator=(
        StageCompositor&& other) noexcept {
    if (this == &other) return *this;
    canvas_ = other.canvas_;
    video_rect_ = other.video_rect_;
    overlay_ = std::move(other.overlay_);
    active_surface_ = &overlay_;
    source_bitmap_cache_ = std::move(other.source_bitmap_cache_);
    defer_d2d_interop_flush_ = other.defer_d2d_interop_flush_;
    return *this;
}

bool StageCompositor::configure(
        const StageCanvas& canvas, void* native_d3d11_device) {
    if (!valid_canvas(canvas)) return false;
    if (!source_bitmap_cache_) {
        source_bitmap_cache_ = std::make_unique<SourceBitmapCache>();
    } else {
        source_bitmap_cache_->clear();
    }
    const bool resized = native_d3d11_device
        ? overlay_.resize_on_device(
            native_d3d11_device, canvas.width, canvas.height)
        : overlay_.resize(canvas.width, canvas.height);
    if (!resized) return false;
    canvas_ = canvas;
    video_rect_ = PixelRect{0, 0, canvas_.width, canvas_.height};
    active_surface_ = &overlay_;
    return true;
}

const StageCanvas& StageCompositor::canvas() const noexcept {
    return canvas_;
}

void StageCompositor::set_defer_d2d_interop_flush(bool defer) noexcept {
    defer_d2d_interop_flush_ = defer;
}

bool StageCompositor::begin_frame(
    std::uint32_t source_width, std::uint32_t source_height) {
    if (!valid_canvas(canvas_)) return false;
    if (!overlay_.resize(canvas_.width, canvas_.height)) return false;
    active_surface_ = &overlay_;
    video_rect_ = letterbox_rect(source_width, source_height, canvas_);
    return clear_overlay();
}

bool StageCompositor::begin_frame_on(D3D11TextureSurface& target,
        std::uint32_t source_width, std::uint32_t source_height,
        Color clear_color) {
    if (!prepare_frame_on(target, source_width, source_height)) return false;
    return clear_overlay(clear_color);
}

bool StageCompositor::prepare_frame_on(D3D11TextureSurface& target,
        std::uint32_t source_width, std::uint32_t source_height) {
    if (!valid_canvas(canvas_)) return false;
    if (!target.resize_on_device(overlay_.native_d3d_device(),
            canvas_.width, canvas_.height)) return false;
    active_surface_ = &target;
    video_rect_ = letterbox_rect(source_width, source_height, canvas_);
    auto* context = static_cast<ID2D1DeviceContext*>(
        target.native_render_target());
    if (!context) return false;
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    return true;
}

bool StageCompositor::prepare_active_surface() {
    if (!active_surface_ || !valid_canvas(canvas_)) return false;
    if (active_surface_ == &overlay_) {
        return overlay_.resize(canvas_.width, canvas_.height);
    }
    return active_surface_->resize_on_device(overlay_.native_d3d_device(),
        canvas_.width, canvas_.height);
}

StageCompositorFrame StageCompositor::current_frame() noexcept {
    auto* surface = active_surface_ ? active_surface_ : &overlay_;
    return StageCompositorFrame{
        .canvas = canvas_,
        .video_rect = video_rect_,
        .overlay_render_target = surface->native_render_target(),
        .overlay_texture = surface->native_texture(),
        .overlay_dxgi_surface = surface->native_dxgi_surface(),
        .d3d_device = surface->native_d3d_device(),
    };
}

PixelRect StageCompositor::letterbox_rect(
    std::uint32_t source_width,
    std::uint32_t source_height,
    const StageCanvas& canvas) {
    if (!valid_canvas(canvas)) return {};
    if (source_width == 0 || source_height == 0) {
        return PixelRect{0, 0, canvas.width, canvas.height};
    }

    const auto scale = std::min(
        static_cast<double>(canvas.width) / source_width,
        static_cast<double>(canvas.height) / source_height);
    auto output_width = static_cast<std::uint32_t>(
        std::max(1.0, std::round(source_width * scale)));
    auto output_height = static_cast<std::uint32_t>(
        std::max(1.0, std::round(source_height * scale)));
    output_width = std::min(output_width, canvas.width);
    output_height = std::min(output_height, canvas.height);
    return PixelRect{
        (canvas.width - output_width) / 2U,
        (canvas.height - output_height) / 2U,
        output_width,
        output_height,
    };
}

bool StageCompositor::clear_overlay(Color color) {
    if (!prepare_active_surface()) return false;
    auto& surface = *active_surface_;
    auto* target = static_cast<ID2D1DeviceContext*>(
        surface.native_render_target());
    if (!target) return false;
    // Overlay renderers use their own coordinate transforms.  Every Stage
    // frame starts in physical canvas coordinates so a previous overlay
    // cannot scale or translate the next video frame.
    target->SetTransform(D2D1::Matrix3x2F::Identity());
    surface.begin_draw();
    surface.clear(color);
    if (!surface.end_draw()) return false;
    // Generic consumers keep the eager boundary. Dedicated pipelines can
    // defer this until their first actual D2D -> D3D transition.
    if (!defer_d2d_interop_flush_) surface.flush_d2d();
    return true;
}

bool StageCompositor::composite_video_texture(
    void* d3d11_texture_2d,
    std::uint32_t source_width,
    std::uint32_t source_height) {
    if (!d3d11_texture_2d) return false;
    if (!valid_canvas(canvas_)) return false;
    if (!prepare_active_surface()) return false;

    auto* target = static_cast<ID2D1DeviceContext*>(
        active_surface_->native_render_target());
    if (!target) return false;

    // Recalculate letterbox for this source.
    video_rect_ = letterbox_rect(source_width, source_height, canvas_);
    if (video_rect_.width == 0 || video_rect_.height == 0) return false;

    auto* src_tex = static_cast<ID3D11Texture2D*>(d3d11_texture_2d);
    auto* source_bitmap = source_bitmap_cache_
        ? source_bitmap_cache_->bitmap_for(target, src_tex) : nullptr;
    if (!source_bitmap) return false;

    const auto destination = D2D1::RectF(
        static_cast<float>(video_rect_.x),
        static_cast<float>(video_rect_.y),
        static_cast<float>(video_rect_.x + video_rect_.width),
        static_cast<float>(video_rect_.y + video_rect_.height));
    target->SetTransform(D2D1::Matrix3x2F::Identity());
    target->BeginDraw();
    target->DrawBitmap(source_bitmap, destination, 1.0F,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    const auto result = target->EndDraw();
    if (!defer_d2d_interop_flush_) target->Flush();
    return SUCCEEDED(result);
}

bool StageCompositor::upload_video_bgra(
    const void* pixels,
    std::uint32_t source_width,
    std::uint32_t source_height) {
    if (!pixels || source_width == 0 || source_height == 0) return false;
    if (!valid_canvas(canvas_)) return false;
    if (!prepare_active_surface()) return false;

    auto* d3d_device = static_cast<ID3D11Device*>(
        active_surface_->native_d3d_device());
    if (!d3d_device) return false;

    // Create a CPU-filled default texture that can be drawn/scaled by D2D.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = source_width;
    desc.Height = source_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels;
    initial.SysMemPitch = source_width * 4U;
    ID3D11Texture2D* source{};
    auto hr = d3d_device->CreateTexture2D(&desc, &initial, &source);
    if (FAILED(hr)) return false;
    const bool ok = composite_video_texture(
        source, source_width, source_height);
    source->Release();
    return ok;
}

D3D11TextureSurface& StageCompositor::overlay_surface() noexcept {
    return active_surface_ ? *active_surface_ : overlay_;
}

const D3D11TextureSurface& StageCompositor::overlay_surface() const noexcept {
    return active_surface_ ? *active_surface_ : overlay_;
}

} // namespace kirakara::show::win32
