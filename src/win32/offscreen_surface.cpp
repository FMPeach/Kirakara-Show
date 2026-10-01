#include "kirakara/show/win32/offscreen_surface.hpp"

#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>

#include <algorithm>
#include <utility>

namespace kirakara::show::win32 {
namespace {

template <typename T>
void release(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

} // namespace

struct OffscreenSurface::Impl {
    ID2D1Factory* d2d{};
    IWICImagingFactory* wic{};
    IWICBitmap* bitmap{};
    ID2D1RenderTarget* target{};
    std::uint32_t width{};
    std::uint32_t height{};
    bool drawing{};
    bool owns_com{};

    Impl() {
        const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        owns_com = com == S_OK || com == S_FALSE;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d);
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&wic));
    }

    ~Impl() {
        release(target);
        release(bitmap);
        release(wic);
        release(d2d);
        if (owns_com) CoUninitialize();
    }

    void reset_pixels() {
        drawing = false;
        release(target);
        release(bitmap);
        width = 0;
        height = 0;
    }
};

OffscreenSurface::OffscreenSurface() : impl_(std::make_unique<Impl>()) {}
OffscreenSurface::~OffscreenSurface() = default;
OffscreenSurface::OffscreenSurface(OffscreenSurface&&) noexcept = default;
OffscreenSurface& OffscreenSurface::operator=(OffscreenSurface&&) noexcept = default;

bool OffscreenSurface::resize(std::uint32_t width, std::uint32_t height) {
    if (!impl_ || !impl_->d2d || !impl_->wic || width == 0 || height == 0) {
        return false;
    }
    if (impl_->width == width && impl_->height == height && impl_->target) {
        return true;
    }
    impl_->reset_pixels();
    if (FAILED(impl_->wic->CreateBitmap(width, height,
            GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
            &impl_->bitmap))) {
        return false;
    }
    const auto properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0F, 96.0F, D2D1_RENDER_TARGET_USAGE_NONE,
        D2D1_FEATURE_LEVEL_DEFAULT);
    if (FAILED(impl_->d2d->CreateWicBitmapRenderTarget(
            impl_->bitmap, properties, &impl_->target))) {
        impl_->reset_pixels();
        return false;
    }
    impl_->width = width;
    impl_->height = height;
    return true;
}

void OffscreenSurface::begin_draw() {
    if (!impl_ || !impl_->target || impl_->drawing) return;
    impl_->target->BeginDraw();
    impl_->drawing = true;
}

bool OffscreenSurface::end_draw() {
    if (!impl_ || !impl_->target || !impl_->drawing) return false;
    impl_->drawing = false;
    return SUCCEEDED(impl_->target->EndDraw());
}

void OffscreenSurface::clear(Color color) {
    if (!impl_ || !impl_->target) return;
    impl_->target->Clear(D2D1::ColorF(color.r, color.g, color.b, color.a));
}

void* OffscreenSurface::native_render_target() noexcept {
    return impl_ ? impl_->target : nullptr;
}

const void* OffscreenSurface::native_render_target() const noexcept {
    return impl_ ? impl_->target : nullptr;
}

bool OffscreenSurface::read_rgba8_into(
    std::uint8_t* buffer, std::size_t buffer_size) const {
    if (!buffer || !impl_ || !impl_->bitmap || impl_->drawing
            || impl_->width == 0 || impl_->height == 0) return false;
    const auto required =
        static_cast<std::size_t>(impl_->width) * impl_->height * 4U;
    if (buffer_size < required) return false;

    WICRect area{0, 0, static_cast<INT>(impl_->width),
        static_cast<INT>(impl_->height)};
    IWICBitmapLock* lock{};
    if (FAILED(impl_->bitmap->Lock(&area, WICBitmapLockRead, &lock))) {
        return false;
    }
    UINT byte_count{};
    UINT stride{};
    BYTE* source{};
    lock->GetStride(&stride);
    lock->GetDataPointer(&byte_count, &source);
    if (source) {
        for (std::uint32_t y = 0; y < impl_->height; ++y) {
            const auto* row = source + static_cast<std::size_t>(y) * stride;
            auto* output = buffer
                + static_cast<std::size_t>(y) * impl_->width * 4U;
            for (std::uint32_t x = 0; x < impl_->width; ++x) {
                output[x * 4U + 0U] = row[x * 4U + 2U];
                output[x * 4U + 1U] = row[x * 4U + 1U];
                output[x * 4U + 2U] = row[x * 4U + 0U];
                output[x * 4U + 3U] = row[x * 4U + 3U];
            }
        }
    }
    release(lock);
    return source != nullptr;
}

std::vector<std::uint8_t> OffscreenSurface::read_rgba8() const {
    if (!impl_ || impl_->width == 0 || impl_->height == 0) return {};
    std::vector<std::uint8_t> result(
        static_cast<std::size_t>(impl_->width) * impl_->height * 4U);
    if (result.empty()) return {};
    if (!read_rgba8_into(result.data(), result.size())) return {};
    return result;
}

bool OffscreenSurface::save_png(std::wstring_view path) const {
    if (!impl_ || !impl_->wic || !impl_->bitmap || impl_->drawing
            || path.empty()) return false;
    IWICStream* stream{};
    IWICBitmapEncoder* encoder{};
    IWICBitmapFrameEncode* frame{};
    IPropertyBag2* properties{};
    bool success = false;
    const std::wstring owned_path{path};
    if (SUCCEEDED(impl_->wic->CreateStream(&stream))
            && SUCCEEDED(stream->InitializeFromFilename(
                owned_path.c_str(), GENERIC_WRITE))
            && SUCCEEDED(impl_->wic->CreateEncoder(GUID_ContainerFormatPng,
                nullptr, &encoder))
            && SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache))
            && SUCCEEDED(encoder->CreateNewFrame(&frame, &properties))
            && SUCCEEDED(frame->Initialize(properties))
            && SUCCEEDED(frame->SetSize(impl_->width, impl_->height))) {
        auto pixel_format = GUID_WICPixelFormat32bppPBGRA;
        if (SUCCEEDED(frame->SetPixelFormat(&pixel_format))
                && SUCCEEDED(frame->WriteSource(impl_->bitmap, nullptr))
                && SUCCEEDED(frame->Commit())
                && SUCCEEDED(encoder->Commit())) {
            success = true;
        }
    }
    release(properties);
    release(frame);
    release(encoder);
    release(stream);
    return success;
}

std::uint32_t OffscreenSurface::width() const noexcept {
    return impl_ ? impl_->width : 0;
}

std::uint32_t OffscreenSurface::height() const noexcept {
    return impl_ ? impl_->height : 0;
}

} // namespace kirakara::show::win32
