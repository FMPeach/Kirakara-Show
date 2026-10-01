#include "kirakara/show/win32/d3d11_texture_surface.hpp"

#include <windows.h>
#include <d2d1_1.h>
#include <d3d10.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cassert>
#include <chrono>
#include <cstring>
#include <thread>
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

struct D3D11TextureSurface::Impl {
    ID3D11Device* d3d_device{};
    ID3D11DeviceContext* d3d_context{};
    ID2D1Factory1* d2d_factory{};
    ID2D1Device* d2d_device{};
    ID2D1DeviceContext* d2d_context{};
    ID2D1SolidColorBrush* copy_brush{};
    ID3D11Texture2D* texture{};
    IDXGISurface* dxgi_surface{};
    ID2D1Bitmap1* target_bitmap{};
    mutable ID3D11Texture2D* staging{};
    std::uint32_t width{};
    std::uint32_t height{};
    bool shared{};
    bool hardware{};
    DWORD producer_thread_id{};

    ~Impl() { shutdown(); }

    void release_target() {
        if (d2d_context) d2d_context->SetTarget(nullptr);
        release(target_bitmap);
        release(dxgi_surface);
        release(texture);
        release(staging);
        width = 0;
        height = 0;
        shared = false;
    }

    void shutdown() {
        release_target();
        release(copy_brush);
        release(d2d_context);
        release(d2d_device);
        release(d2d_factory);
        release(d3d_context);
        release(d3d_device);
        hardware = false;
        producer_thread_id = 0;
    }

    void retire_producer_access() noexcept {
        if (!d2d_context && !d2d_device && !d2d_factory) return;
        // Direct2D was created as single-threaded. This method is intentionally
        // called before a Stage pool generation leaves the producer thread.
        // Outstanding Flutter/native/Cast leases keep only the immutable D3D
        // texture alive after this point.
        assert(producer_thread_id == 0
            || producer_thread_id == GetCurrentThreadId());
        if (d2d_context) d2d_context->SetTarget(nullptr);
        release(target_bitmap);
        release(dxgi_surface);
        release(copy_brush);
        release(d2d_context);
        release(d2d_device);
        release(d2d_factory);
        producer_thread_id = 0;
    }

    bool initialize_device(ID3D11Device* requested_device = nullptr) {
        if (d3d_device && d2d_context
                && (!requested_device || requested_device == d3d_device)) {
            return true;
        }
        if (d3d_device) shutdown();

        HRESULT result = S_OK;
        if (requested_device) {
            d3d_device = requested_device;
            d3d_device->AddRef();
            d3d_device->GetImmediateContext(&d3d_context);
            hardware = d3d_context != nullptr;
            if (!d3d_context) {
                shutdown();
                return false;
            }
        }
        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        if (!requested_device) {
            D3D_FEATURE_LEVEL selected{};
            constexpr UINT device_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT
                | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
            result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
                nullptr, device_flags, levels,
                static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                &d3d_device, &selected, &d3d_context);
            hardware = SUCCEEDED(result);
            if (FAILED(result)) {
                result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
                    nullptr, device_flags, levels,
                    static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                    &d3d_device, &selected, &d3d_context);
            }
            if (FAILED(result)) return false;
        }

        // The Cast decoder and Stage compositor intentionally share this
        // device across their worker/render threads. Protect immediate-context
        // access as recommended for MF D3D11 decode pipelines.
        ID3D10Multithread* multithread{};
        if (SUCCEEDED(d3d_device->QueryInterface(
                IID_PPV_ARGS(&multithread)))) {
            multithread->SetMultithreadProtected(TRUE);
        }
        release(multithread);

        IDXGIDevice* dxgi_device{};
        result = d3d_device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
        D2D1_FACTORY_OPTIONS options{};
        if (SUCCEEDED(result)) {
            result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                options, &d2d_factory);
        }
        if (SUCCEEDED(result)) {
            result = d2d_factory->CreateDevice(dxgi_device, &d2d_device);
        }
        if (SUCCEEDED(result)) {
            result = d2d_device->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_context);
        }
        release(dxgi_device);
        if (FAILED(result)) {
            shutdown();
            return false;
        }
        producer_thread_id = GetCurrentThreadId();
        return true;
    }

    bool initialize_device_in_resource_domain(const Impl& domain) {
        if (!domain.d3d_device || !domain.d2d_factory || !domain.d2d_device) {
            return false;
        }
        if (d3d_device == domain.d3d_device
                && d2d_device == domain.d2d_device && d2d_context) {
            return true;
        }
        shutdown();

        d3d_device = domain.d3d_device;
        d3d_device->AddRef();
        d3d_device->GetImmediateContext(&d3d_context);
        d2d_factory = domain.d2d_factory;
        d2d_factory->AddRef();
        d2d_device = domain.d2d_device;
        d2d_device->AddRef();
        const auto result = d2d_device->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_context);
        if (!d3d_context || FAILED(result)) {
            shutdown();
            return false;
        }
        hardware = domain.hardware;
        producer_thread_id = GetCurrentThreadId();
        return true;
    }

    bool resize(std::uint32_t new_width, std::uint32_t new_height,
            ID3D11Device* requested_device = nullptr,
            bool request_shared = false,
            const Impl* resource_domain = nullptr) {
        if (new_width == 0 || new_height == 0
                || !(resource_domain
                    ? initialize_device_in_resource_domain(*resource_domain)
                    : initialize_device(requested_device))) return false;
        // A shareable texture is also a valid ordinary render target. Do not
        // silently replace a StageFramePool slot with a non-shareable texture
        // when the compositor begins drawing into it; that would leave Sinks
        // holding the stale handle of the destroyed resource.
        if (texture && width == new_width && height == new_height
                && (shared || !request_shared)) return true;
        release_target();

        D3D11_TEXTURE2D_DESC description{};
        description.Width = new_width;
        description.Height = new_height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET
            | D3D11_BIND_SHADER_RESOURCE;
        if (request_shared) {
            description.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        }
        auto result = d3d_device->CreateTexture2D(&description, nullptr, &texture);
        IDXGISurface* surface{};
        if (SUCCEEDED(result)) result = texture->QueryInterface(IID_PPV_ARGS(&surface));
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0F, 96.0F);
        if (SUCCEEDED(result)) {
            result = d2d_context->CreateBitmapFromDxgiSurface(surface,
                &properties, &target_bitmap);
        }
        if (SUCCEEDED(result)) {
            dxgi_surface = surface;
            surface = nullptr;
        }
        release(surface);
        if (FAILED(result)) {
            release_target();
            return false;
        }
        d2d_context->SetTarget(target_bitmap);
        d2d_context->SetDpi(96.0F, 96.0F);
        width = new_width;
        height = new_height;
        shared = request_shared;
        return true;
    }

    std::vector<std::uint8_t> read_rgba8() const {
        if (!texture || !d3d_device || !d3d_context || width == 0 || height == 0) {
            return {};
        }
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        description.MiscFlags = 0;
        ID3D11Texture2D* staging{};
        if (FAILED(d3d_device->CreateTexture2D(&description, nullptr, &staging))) {
            return {};
        }
        d3d_context->CopyResource(staging, texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(d3d_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            release(staging);
            return {};
        }
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(width) * height * 4U);
        for (std::uint32_t y = 0; y < height; ++y) {
            const auto* source = static_cast<const std::uint8_t*>(mapped.pData)
                + static_cast<std::size_t>(y) * mapped.RowPitch;
            auto* destination = pixels.data()
                + static_cast<std::size_t>(y) * width * 4U;
            for (std::uint32_t x = 0; x < width; ++x) {
                destination[x * 4U] = source[x * 4U + 2U];
                destination[x * 4U + 1U] = source[x * 4U + 1U];
                destination[x * 4U + 2U] = source[x * 4U];
                destination[x * 4U + 3U] = source[x * 4U + 3U];
            }
        }
        d3d_context->Unmap(staging, 0);
        release(staging);
        return pixels;
    }

    bool ensure_staging() const {
        if (!texture || !d3d_device || width == 0 || height == 0) return false;
        if (staging) {
            D3D11_TEXTURE2D_DESC d{};
            staging->GetDesc(&d);
            if (d.Width == width && d.Height == height) return true;
            release(staging);
        }
        D3D11_TEXTURE2D_DESC d{};
        texture->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        return SUCCEEDED(d3d_device->CreateTexture2D(&d, nullptr, &staging));
    }

    bool read_bgra8_into(void* dst) const {
        if (!dst || !ensure_staging()) return false;
        d3d_context->CopyResource(staging, texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(d3d_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
            return false;
        auto* dest = static_cast<std::uint8_t*>(dst);
        for (std::uint32_t y = 0; y < height; ++y) {
            const auto* src = static_cast<const std::uint8_t*>(mapped.pData)
                + static_cast<std::size_t>(y) * mapped.RowPitch;
            std::memcpy(dest + static_cast<std::size_t>(y) * width * 4U,
                src, static_cast<std::size_t>(width) * 4U);
        }
        d3d_context->Unmap(staging, 0);
        return true;
    }

    bool synchronize() const {
        if (!d3d_device || !d3d_context) return false;
        D3D11_QUERY_DESC description{};
        description.Query = D3D11_QUERY_EVENT;
        ID3D11Query* query{};
        if (FAILED(d3d_device->CreateQuery(&description, &query))) return false;
        d3d_context->End(query);
        d3d_context->Flush();
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(10);
        HRESULT result = S_FALSE;
        while (result == S_FALSE && std::chrono::steady_clock::now() < deadline) {
            result = d3d_context->GetData(query, nullptr, 0, 0);
            if (result == S_FALSE) std::this_thread::yield();
        }
        release(query);
        return result == S_OK;
    }

    bool clear_rect(float left, float top, float right, float bottom,
            Color color) {
        if (!d2d_context || !(right > left) || !(bottom > top)) {
            return false;
        }
        if (!copy_brush && FAILED(d2d_context->CreateSolidColorBrush(
                D2D1::ColorF(color.r, color.g, color.b, color.a),
                &copy_brush))) {
            return false;
        }
        copy_brush->SetColor(D2D1::ColorF(
            color.r, color.g, color.b, color.a));
        D2D1_MATRIX_3X2_F transform{};
        d2d_context->GetTransform(&transform);
        const auto primitive_blend = d2d_context->GetPrimitiveBlend();
        d2d_context->SetTransform(D2D1::Matrix3x2F::Identity());
        d2d_context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
        d2d_context->FillRectangle(
            D2D1::RectF(left, top, right, bottom), copy_brush);
        d2d_context->SetPrimitiveBlend(primitive_blend);
        d2d_context->SetTransform(transform);
        return true;
    }
};

D3D11TextureSurface::D3D11TextureSurface() : impl_(std::make_unique<Impl>()) {}
D3D11TextureSurface::~D3D11TextureSurface() = default;
D3D11TextureSurface::D3D11TextureSurface(D3D11TextureSurface&&) noexcept = default;
D3D11TextureSurface& D3D11TextureSurface::operator=(
    D3D11TextureSurface&&) noexcept = default;

bool D3D11TextureSurface::resize(std::uint32_t width, std::uint32_t height) {
    return impl_ && impl_->resize(width, height);
}

bool D3D11TextureSurface::resize_on_device(void* native_d3d11_device,
        std::uint32_t width, std::uint32_t height) {
    return impl_ && native_d3d11_device
        && impl_->resize(width, height,
            static_cast<ID3D11Device*>(native_d3d11_device));
}

bool D3D11TextureSurface::resize_shared_on_device(
        void* native_d3d11_device,
        std::uint32_t width,
        std::uint32_t height) {
    return impl_ && native_d3d11_device
        && impl_->resize(width, height,
            static_cast<ID3D11Device*>(native_d3d11_device), true);
}

bool D3D11TextureSurface::resize_shared_in_resource_domain(
        const D3D11TextureSurface& resource_domain,
        std::uint32_t width,
        std::uint32_t height) {
    return impl_ && resource_domain.impl_
        && impl_->resize(width, height, nullptr, true,
            resource_domain.impl_.get());
}

void* D3D11TextureSurface::native_render_target() noexcept {
    return impl_ ? impl_->d2d_context : nullptr;
}

void* D3D11TextureSurface::native_d3d_device() noexcept {
    return impl_ ? impl_->d3d_device : nullptr;
}

void* D3D11TextureSurface::native_dxgi_surface() noexcept {
    return impl_ ? impl_->dxgi_surface : nullptr;
}

void* D3D11TextureSurface::native_texture() noexcept {
    return impl_ ? impl_->texture : nullptr;
}

const void* D3D11TextureSurface::native_texture() const noexcept {
    return impl_ ? impl_->texture : nullptr;
}

bool D3D11TextureSurface::copy_from(
        const D3D11TextureSurface& source) noexcept {
    if (!impl_ || !source.impl_ || !impl_->d3d_context
            || !impl_->texture || !source.impl_->texture
            || impl_->d3d_device != source.impl_->d3d_device
            || impl_->width != source.impl_->width
            || impl_->height != source.impl_->height) {
        return false;
    }
    D3D11_TEXTURE2D_DESC destination_description{};
    D3D11_TEXTURE2D_DESC source_description{};
    impl_->texture->GetDesc(&destination_description);
    source.impl_->texture->GetDesc(&source_description);
    if (destination_description.Format != source_description.Format
            || destination_description.MipLevels
                != source_description.MipLevels
            || destination_description.ArraySize
                != source_description.ArraySize
            || destination_description.SampleDesc.Count
                != source_description.SampleDesc.Count
            || destination_description.SampleDesc.Quality
                != source_description.SampleDesc.Quality) {
        return false;
    }
    if (impl_->texture != source.impl_->texture) {
        impl_->d3d_context->CopyResource(
            impl_->texture, source.impl_->texture);
    }
    return true;
}

void D3D11TextureSurface::begin_draw() {
    if (impl_ && impl_->d2d_context) impl_->d2d_context->BeginDraw();
}

bool D3D11TextureSurface::end_draw() {
    return impl_ && impl_->d2d_context
        && SUCCEEDED(impl_->d2d_context->EndDraw());
}

void D3D11TextureSurface::flush_d2d() {
    if (impl_ && impl_->d2d_context) {
        impl_->d2d_context->Flush();
    }
}

void D3D11TextureSurface::flush_d3d() {
    if (impl_ && impl_->d3d_context) {
        impl_->d3d_context->Flush();
    }
}

void D3D11TextureSurface::clear(Color color) {
    if (impl_ && impl_->d2d_context)
        impl_->d2d_context->Clear(D2D1::ColorF(color.r, color.g, color.b, color.a));
}

bool D3D11TextureSurface::clear_rect(float left, float top, float right,
        float bottom, Color color) {
    return impl_ && impl_->clear_rect(left, top, right, bottom, color);
}

std::vector<std::uint8_t> D3D11TextureSurface::read_rgba8() const {
    return impl_ ? impl_->read_rgba8() : std::vector<std::uint8_t>{};
}

bool D3D11TextureSurface::read_bgra8_into(void* dst) const {
    return impl_ && impl_->read_bgra8_into(dst);
}

bool D3D11TextureSurface::synchronize() const {
    return impl_ && impl_->synchronize();
}

std::uint32_t D3D11TextureSurface::width() const noexcept {
    return impl_ ? impl_->width : 0;
}

std::uint32_t D3D11TextureSurface::height() const noexcept {
    return impl_ ? impl_->height : 0;
}

bool D3D11TextureSurface::using_hardware_device() const noexcept {
    return impl_ && impl_->hardware;
}

void D3D11TextureSurface::retire_producer_access() noexcept {
    if (impl_) impl_->retire_producer_access();
}

} // namespace kirakara::show::win32
