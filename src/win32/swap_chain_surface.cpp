#include "kirakara/show/win32/swap_chain_surface.hpp"

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>

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

struct SwapChainSurface::Impl {
    HWND hwnd{};
    ID3D11Device* d3d_device{};
    ID3D11DeviceContext* d3d_context{};
    IDXGIFactory2* dxgi_factory{};
    IDXGISwapChain1* swap_chain{};
    IDXGISurface* back_buffer_surface{};
    ID2D1Factory1* d2d_factory{};
    ID2D1Device* d2d_device{};
    ID2D1DeviceContext* d2d_context{};
    ID2D1Bitmap1* target_bitmap{};
    std::uint32_t width{};
    std::uint32_t height{};
    bool hardware{};

    ~Impl() { shutdown(); }

    void release_target() {
        if (d2d_context) d2d_context->SetTarget(nullptr);
        release(target_bitmap);
        release(back_buffer_surface);
        width = 0;
        height = 0;
    }

    void shutdown() {
        release_target();
        release(swap_chain);
        release(dxgi_factory);
        release(d2d_context);
        release(d2d_device);
        release(d2d_factory);
        release(d3d_context);
        release(d3d_device);
        hwnd = nullptr;
        hardware = false;
    }

    bool initialize_device() {
        if (d3d_device && d2d_context && dxgi_factory) return true;
        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL selected{};
        auto result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
            nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
            static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
            &d3d_device, &selected, &d3d_context);
        hardware = SUCCEEDED(result);
        if (FAILED(result)) {
            result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
                nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                &d3d_device, &selected, &d3d_context);
        }
        if (FAILED(result)) return false;

        IDXGIDevice* dxgi_device{};
        IDXGIAdapter* adapter{};
        result = d3d_device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
        if (SUCCEEDED(result)) result = dxgi_device->GetAdapter(&adapter);
        IDXGIFactory2* factory{};
        if (SUCCEEDED(result)) {
            result = adapter->GetParent(IID_PPV_ARGS(&factory));
        }
        if (SUCCEEDED(result)) dxgi_factory = factory;
        else release(factory);

        D2D1_FACTORY_OPTIONS options{};
        if (SUCCEEDED(result)) {
            result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                options, &d2d_factory);
        }
        if (SUCCEEDED(result)) result = d2d_factory->CreateDevice(dxgi_device, &d2d_device);
        if (SUCCEEDED(result)) {
            result = d2d_device->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_context);
        }
        release(adapter);
        release(dxgi_device);
        if (FAILED(result)) {
            shutdown();
            return false;
        }
        return true;
    }

    bool create_or_resize_target(std::uint32_t new_width, std::uint32_t new_height) {
        if (!hwnd || !initialize_device()) return false;
        new_width = std::max<std::uint32_t>(1, new_width);
        new_height = std::max<std::uint32_t>(1, new_height);
        release_target();

        HRESULT result = S_OK;
        if (!swap_chain) {
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = new_width;
            desc.Height = new_height;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            result = dxgi_factory->CreateSwapChainForHwnd(
                d3d_device, hwnd, &desc, nullptr, nullptr, &swap_chain);
            if (SUCCEEDED(result)) {
                dxgi_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
            }
        } else {
            result = swap_chain->ResizeBuffers(0, new_width, new_height,
                DXGI_FORMAT_UNKNOWN, 0);
        }
        if (FAILED(result)) return false;

        result = swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer_surface));
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0F, 96.0F);
        if (SUCCEEDED(result)) {
            result = d2d_context->CreateBitmapFromDxgiSurface(back_buffer_surface,
                &properties, &target_bitmap);
        }
        if (FAILED(result)) {
            release_target();
            return false;
        }
        d2d_context->SetTarget(target_bitmap);
        d2d_context->SetDpi(96.0F, 96.0F);
        width = new_width;
        height = new_height;
        return true;
    }

    bool initialize(HWND target_hwnd) {
        if (!target_hwnd) return false;
        hwnd = target_hwnd;
        RECT rect{};
        GetClientRect(hwnd, &rect);
        return create_or_resize_target(
            static_cast<std::uint32_t>(std::max<LONG>(1, rect.right - rect.left)),
            static_cast<std::uint32_t>(std::max<LONG>(1, rect.bottom - rect.top)));
    }

    bool resize(std::uint32_t new_width, std::uint32_t new_height) {
        if (!swap_chain && !hwnd) return false;
        if (new_width == width && new_height == height && target_bitmap) return true;
        return create_or_resize_target(new_width, new_height);
    }

    bool present(bool wait_for_vsync) {
        if (!swap_chain) return false;
        return SUCCEEDED(swap_chain->Present(wait_for_vsync ? 1U : 0U, 0));
    }
};

SwapChainSurface::SwapChainSurface() : impl_(std::make_unique<Impl>()) {}
SwapChainSurface::~SwapChainSurface() = default;
SwapChainSurface::SwapChainSurface(SwapChainSurface&&) noexcept = default;
SwapChainSurface& SwapChainSurface::operator=(SwapChainSurface&&) noexcept = default;

bool SwapChainSurface::initialize(void* hwnd) {
    return impl_ && impl_->initialize(static_cast<HWND>(hwnd));
}

bool SwapChainSurface::resize(std::uint32_t width, std::uint32_t height) {
    return impl_ && impl_->resize(width, height);
}

void* SwapChainSurface::native_render_target() noexcept {
    return impl_ ? impl_->d2d_context : nullptr;
}

void* SwapChainSurface::native_d3d_device() noexcept {
    return impl_ ? impl_->d3d_device : nullptr;
}

void* SwapChainSurface::native_dxgi_surface() noexcept {
    return impl_ ? impl_->back_buffer_surface : nullptr;
}

bool SwapChainSurface::present(bool wait_for_vsync) {
    return impl_ && impl_->present(wait_for_vsync);
}

void SwapChainSurface::shutdown() {
    if (impl_) impl_->shutdown();
}

std::uint32_t SwapChainSurface::width() const noexcept {
    return impl_ ? impl_->width : 0;
}

std::uint32_t SwapChainSurface::height() const noexcept {
    return impl_ ? impl_->height : 0;
}

bool SwapChainSurface::using_hardware_device() const noexcept {
    return impl_ && impl_->hardware;
}

} // namespace kirakara::show::win32
