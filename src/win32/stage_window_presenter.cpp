#include "kirakara/show/win32/stage_window_presenter.hpp"

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
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

struct SourceBitmap {
    ID3D11Texture2D* texture{};
    ID2D1Bitmap1* bitmap{};
    std::uint32_t width{};
    std::uint32_t height{};

    SourceBitmap() = default;
    SourceBitmap(const SourceBitmap&) = delete;
    SourceBitmap& operator=(const SourceBitmap&) = delete;
    SourceBitmap(SourceBitmap&& other) noexcept
        : texture(std::exchange(other.texture, nullptr)),
          bitmap(std::exchange(other.bitmap, nullptr)),
          width(other.width),
          height(other.height) {}
    SourceBitmap& operator=(SourceBitmap&& other) noexcept {
        if (this == &other) return *this;
        release(bitmap);
        release(texture);
        texture = std::exchange(other.texture, nullptr);
        bitmap = std::exchange(other.bitmap, nullptr);
        width = other.width;
        height = other.height;
        return *this;
    }
    ~SourceBitmap() {
        release(bitmap);
        release(texture);
    }
};

} // namespace

struct StageWindowPresenter::Impl {
    HWND hwnd{};
    ID3D11Device* device{};
    ID3D11DeviceContext* immediate_context{};
    IDXGIFactory2* factory{};
    IDXGISwapChain1* swap_chain{};
    ID3D11Texture2D* back_buffer_texture{};
    IDXGISurface* back_buffer_surface{};
    ID2D1Factory1* d2d_factory{};
    ID2D1Device* d2d_device{};
    ID2D1DeviceContext* d2d_context{};
    ID2D1Bitmap1* target_bitmap{};
    std::vector<SourceBitmap> source_bitmaps;
    StageFrameLease last_frame;
    StageFrameTiming timing{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint64_t output_revision{};
    std::uint64_t presented{};
    HRESULT last_error{S_OK};

    ~Impl() { shutdown(); }

    void release_sources() {
        source_bitmaps.clear();
    }

    void release_target() {
        if (d2d_context) d2d_context->SetTarget(nullptr);
        release(target_bitmap);
        release(back_buffer_surface);
        release(back_buffer_texture);
        output_width = 0;
        output_height = 0;
    }

    void shutdown() {
        last_frame = {};
        timing = {};
        release_sources();
        release_target();
        release(swap_chain);
        release(d2d_context);
        release(d2d_device);
        release(d2d_factory);
        release(factory);
        release(immediate_context);
        release(device);
        hwnd = nullptr;
        presented = 0;
    }

    bool initialize(HWND target, ID3D11Device* requested_device) {
        if (!target || !requested_device) {
            last_error = E_INVALIDARG;
            return false;
        }
        if (hwnd == target && device == requested_device && swap_chain) {
            return true;
        }
        shutdown();
        hwnd = target;
        device = requested_device;
        device->AddRef();
        device->GetImmediateContext(&immediate_context);
        if (!immediate_context) {
            last_error = E_FAIL;
            shutdown();
            return false;
        }

        IDXGIDevice* dxgi_device{};
        IDXGIAdapter* adapter{};
        auto result = device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
        if (SUCCEEDED(result)) result = dxgi_device->GetAdapter(&adapter);
        if (SUCCEEDED(result)) {
            result = adapter->GetParent(IID_PPV_ARGS(&factory));
        }

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
        release(adapter);
        release(dxgi_device);
        if (FAILED(result)) {
            last_error = result;
            shutdown();
            return false;
        }

        RECT rect{};
        GetClientRect(hwnd, &rect);
        return resize(
            static_cast<std::uint32_t>(
                std::max<LONG>(1, rect.right - rect.left)),
            static_cast<std::uint32_t>(
                std::max<LONG>(1, rect.bottom - rect.top)));
    }

    bool resize(std::uint32_t width, std::uint32_t height) {
        if (!hwnd || !device || !factory || !d2d_context) {
            last_error = E_HANDLE;
            return false;
        }
        width = std::max<std::uint32_t>(1, width);
        height = std::max<std::uint32_t>(1, height);
        if (swap_chain && target_bitmap
                && width == output_width && height == output_height) {
            return true;
        }

        release_target();
        HRESULT result = S_OK;
        if (!swap_chain) {
            DXGI_SWAP_CHAIN_DESC1 description{};
            description.Width = width;
            description.Height = height;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            description.BufferCount = 2;
            description.Scaling = DXGI_SCALING_STRETCH;
            description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            result = factory->CreateSwapChainForHwnd(
                device, hwnd, &description, nullptr, nullptr, &swap_chain);
            if (SUCCEEDED(result)) {
                factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
            }
        } else {
            result = swap_chain->ResizeBuffers(
                0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        }
        if (FAILED(result)) {
            last_error = result;
            return false;
        }

        result = swap_chain->GetBuffer(
            0, IID_PPV_ARGS(&back_buffer_texture));
        if (SUCCEEDED(result)) {
            result = back_buffer_texture->QueryInterface(
                IID_PPV_ARGS(&back_buffer_surface));
        }
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_IGNORE),
            96.0F,
            96.0F);
        if (SUCCEEDED(result)) {
            result = d2d_context->CreateBitmapFromDxgiSurface(
                back_buffer_surface, &properties, &target_bitmap);
        }
        if (FAILED(result)) {
            last_error = result;
            release_target();
            return false;
        }
        d2d_context->SetTarget(target_bitmap);
        d2d_context->SetDpi(96.0F, 96.0F);
        output_width = width;
        output_height = height;
        ++output_revision;
        last_error = S_OK;
        return true;
    }

    bool recover_output() {
        if (!hwnd || !device || !factory || !d2d_context) {
            last_error = E_HANDLE;
            return false;
        }
        RECT client{};
        if (!GetClientRect(hwnd, &client)) {
            last_error = HRESULT_FROM_WIN32(GetLastError());
            return false;
        }
        const auto width = static_cast<std::uint32_t>(
            std::max<LONG>(1, client.right - client.left));
        const auto height = static_cast<std::uint32_t>(
            std::max<LONG>(1, client.bottom - client.top));
        release_target();
        release(swap_chain);
        return resize(width, height);
    }

    SourceBitmap* source_bitmap(ID3D11Texture2D* texture) {
        for (auto& entry : source_bitmaps) {
            if (entry.texture == texture) return &entry;
        }
        ID3D11Device* source_device{};
        texture->GetDevice(&source_device);
        const bool same_device = source_device == device;
        release(source_device);
        if (!same_device) return nullptr;

        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM
                || description.Width == 0 || description.Height == 0) {
            return nullptr;
        }
        IDXGISurface* surface{};
        auto result = texture->QueryInterface(IID_PPV_ARGS(&surface));
        ID2D1Bitmap1* bitmap{};
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(description.Format,
                D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0F,
            96.0F);
        if (SUCCEEDED(result)) {
            result = d2d_context->CreateBitmapFromDxgiSurface(
                surface, &properties, &bitmap);
        }
        release(surface);
        if (FAILED(result) || !bitmap) {
            release(bitmap);
            return nullptr;
        }
        texture->AddRef();
        source_bitmaps.emplace_back();
        auto& entry = source_bitmaps.back();
        entry.texture = texture;
        entry.bitmap = bitmap;
        entry.width = description.Width;
        entry.height = description.Height;
        return &entry;
    }

    bool draw(const StageFrameLease* frame, bool wait_for_vsync) {
        if (!swap_chain || !d2d_context || !target_bitmap) {
            last_error = E_HANDLE;
            return false;
        }
        SourceBitmap* source{};
        if (frame && *frame && (*frame)->native_resource) {
            source = source_bitmap(
                static_cast<ID3D11Texture2D*>((*frame)->native_resource));
            if (!source) {
                last_error = E_FAIL;
                return false;
            }
        }

        if (source && source->width == output_width
                && source->height == output_height
                && back_buffer_texture) {
            immediate_context->CopyResource(
                back_buffer_texture, source->texture);
            const auto present_result = swap_chain->Present(
                wait_for_vsync ? 1U : 0U, 0);
            if (FAILED(present_result)) {
                last_error = present_result;
                return false;
            }
            ++presented;
            last_error = S_OK;
            return true;
        }

        d2d_context->BeginDraw();
        d2d_context->Clear(D2D1::ColorF(D2D1::ColorF::Black));
        if (source) {
            const auto destination = D2D1::RectF(
                0.0F,
                0.0F,
                static_cast<float>(output_width),
                static_cast<float>(output_height));
            const auto source_rect = D2D1::RectF(
                0.0F,
                0.0F,
                static_cast<float>(source->width),
                static_cast<float>(source->height));
            d2d_context->DrawBitmap(
                source->bitmap,
                destination,
                1.0F,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                source_rect);
        }
        const auto draw_result = d2d_context->EndDraw();
        if (FAILED(draw_result)) {
            last_error = draw_result;
            return false;
        }
        const auto present_result = swap_chain->Present(
            wait_for_vsync ? 1U : 0U, 0);
        if (FAILED(present_result)) {
            last_error = present_result;
            return false;
        }
        ++presented;
        last_error = S_OK;
        return true;
    }

    bool present(const StageFrameLease& frame, bool wait_for_vsync) {
        if (!frame || frame->profile.pixel_format != StagePixelFormat::bgra8) {
            return false;
        }
        if (!draw(&frame, wait_for_vsync)) return false;
        last_frame = frame;
        timing = frame->timing;
        return true;
    }
};

StageWindowPresenter::StageWindowPresenter()
    : impl_(std::make_unique<Impl>()) {}
StageWindowPresenter::~StageWindowPresenter() = default;
StageWindowPresenter::StageWindowPresenter(StageWindowPresenter&&) noexcept =
    default;
StageWindowPresenter& StageWindowPresenter::operator=(
    StageWindowPresenter&&) noexcept = default;

bool StageWindowPresenter::initialize(
        void* hwnd, void* native_d3d11_device) {
    return impl_ && impl_->initialize(
        static_cast<HWND>(hwnd),
        static_cast<ID3D11Device*>(native_d3d11_device));
}

bool StageWindowPresenter::resize(
        std::uint32_t width, std::uint32_t height) {
    return impl_ && impl_->resize(width, height);
}

bool StageWindowPresenter::present(
        const StageFrameLease& frame, bool wait_for_vsync) {
    return impl_ && impl_->present(frame, wait_for_vsync);
}

bool StageWindowPresenter::repeat_last(bool wait_for_vsync) {
    return impl_ && impl_->last_frame
        && impl_->draw(&impl_->last_frame, wait_for_vsync);
}

bool StageWindowPresenter::present_black(bool wait_for_vsync) {
    if (!impl_ || !impl_->draw(nullptr, wait_for_vsync)) return false;
    impl_->last_frame = {};
    impl_->timing = {};
    return true;
}

bool StageWindowPresenter::recover_output() {
    return impl_ && impl_->recover_output();
}

void StageWindowPresenter::release_frame() {
    if (!impl_) return;
    impl_->last_frame = {};
    impl_->timing = {};
    impl_->release_sources();
}

void StageWindowPresenter::shutdown() {
    if (impl_) impl_->shutdown();
}

bool StageWindowPresenter::initialized() const noexcept {
    return impl_ && impl_->swap_chain;
}

std::uint32_t StageWindowPresenter::width() const noexcept {
    return impl_ ? impl_->output_width : 0;
}

std::uint32_t StageWindowPresenter::height() const noexcept {
    return impl_ ? impl_->output_height : 0;
}

std::uint64_t StageWindowPresenter::output_revision() const noexcept {
    return impl_ ? impl_->output_revision : 0;
}

std::uint64_t StageWindowPresenter::presented_frames() const noexcept {
    return impl_ ? impl_->presented : 0;
}

StageFrameTiming StageWindowPresenter::last_timing() const noexcept {
    return impl_ ? impl_->timing : StageFrameTiming{};
}

std::int32_t StageWindowPresenter::last_error_code() const noexcept {
    return impl_ ? static_cast<std::int32_t>(impl_->last_error)
        : static_cast<std::int32_t>(E_POINTER);
}

} // namespace kirakara::show::win32
