#include "physical_hud_output.h"

#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cmath>
#include <utility>

using Microsoft::WRL::ComPtr;

struct PhysicalHudOutput::Impl {
    HWND window{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDCompositionDevice> composition;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDXGISwapChain1> swap_chain;
    ComPtr<ID3D11Texture2D> source;
    std::uint32_t width{}, height{};
    std::uint64_t revision{}, submitted{};
    float x{}, y{};
    bool visible{}, has_content{};
    HRESULT error{S_OK};

    bool check(HRESULT hr) noexcept {
        error = hr;
        return SUCCEEDED(hr);
    }

    void shutdown() noexcept {
        if (target) {
            target->SetRoot(nullptr);
            if (composition) composition->Commit();
        }
        source.Reset();
        visual.Reset();
        swap_chain.Reset();
        target.Reset();
        composition.Reset();
        factory.Reset();
        context.Reset();
        device.Reset();
        window = nullptr;
        width = height = 0;
        revision = submitted = 0;
        visible = has_content = false;
    }

    bool initialize(HWND hwnd, ID3D11Device* requested_device) {
        if (!hwnd || !requested_device) return check(E_INVALIDARG);
        if (window == hwnd && device.Get() == requested_device && target) return true;
        shutdown();
        window = hwnd;
        device = requested_device;
        device->GetImmediateContext(&context);
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        if (!context || !check(device.As(&dxgi))
                || !check(dxgi->GetAdapter(&adapter))
                || !check(adapter->GetParent(IID_PPV_ARGS(&factory)))
                || !check(DCompositionCreateDevice(dxgi.Get(),
                    __uuidof(IDCompositionDevice),
                    reinterpret_cast<void**>(composition.GetAddressOf())))
                || !check(composition->CreateTargetForHwnd(hwnd, TRUE, &target))
                || !check(composition->CreateVisual(&visual))
                || !check(target->SetRoot(nullptr))
                || !check(composition->Commit())) {
            shutdown();
            return false;
        }
        return true;
    }

    bool present(ID3D11Texture2D* texture, float left, float top,
            std::uint64_t content_revision) {
        if (!target || !texture || !std::isfinite(left) || !std::isfinite(top)) {
            return check(E_INVALIDARG);
        }
        ComPtr<ID3D11Device> input_device;
        texture->GetDevice(&input_device);
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (input_device.Get() != device.Get()
                || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM
                || desc.SampleDesc.Count != 1 || desc.MipLevels != 1
                || desc.ArraySize != 1) return check(E_INVALIDARG);
        const bool resize = !swap_chain || width != desc.Width || height != desc.Height;
        const bool changed = resize || !has_content || source.Get() != texture
            || revision != content_revision;
        if (resize) {
            DXGI_SWAP_CHAIN_DESC1 config{};
            config.Width = desc.Width;
            config.Height = desc.Height;
            config.Format = desc.Format;
            config.SampleDesc.Count = 1;
            config.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            config.BufferCount = 2;
            config.Scaling = DXGI_SCALING_STRETCH;
            config.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            config.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            ComPtr<IDXGISwapChain1> replacement;
            if (!check(factory->CreateSwapChainForComposition(device.Get(),
                    &config, nullptr, &replacement))
                    || !check(visual->SetContent(replacement.Get()))) return false;
            swap_chain = std::move(replacement);
            width = desc.Width;
            height = desc.Height;
        }
        if (changed) {
            ComPtr<ID3D11Texture2D> buffer;
            if (!check(swap_chain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return false;
            context->CopyResource(buffer.Get(), texture);
            if (!check(swap_chain->Present(0, 0))) return false;
            ++submitted;
            source = texture;
            revision = content_revision;
            has_content = true;
        }
        if (changed || !visible || x != left || y != top) {
            if (!check(visual->SetOffsetX(left)) || !check(visual->SetOffsetY(top))
                    || !check(target->SetRoot(visual.Get()))
                    || !check(composition->Commit())) return false;
            x = left;
            y = top;
            visible = true;
        }
        return check(S_OK);
    }
};

PhysicalHudOutput::PhysicalHudOutput() : impl_(std::make_unique<Impl>()) {}
PhysicalHudOutput::~PhysicalHudOutput() { impl_->shutdown(); }
bool PhysicalHudOutput::initialize(void* hwnd, void* device) {
    return impl_->initialize(static_cast<HWND>(hwnd), static_cast<ID3D11Device*>(device));
}
bool PhysicalHudOutput::present(void* texture, float x, float y, std::uint64_t revision) {
    return impl_->present(static_cast<ID3D11Texture2D*>(texture), x, y, revision);
}
bool PhysicalHudOutput::set_visible(bool visible) {
    if (!impl_->visual) return !visible;
    if (impl_->visible == visible) return true;
    if (!impl_->check(impl_->target->SetRoot(visible ? impl_->visual.Get() : nullptr))
            || !impl_->check(impl_->composition->Commit())) return false;
    impl_->visible = visible;
    return true;
}
void PhysicalHudOutput::shutdown() noexcept { impl_->shutdown(); }
bool PhysicalHudOutput::initialized() const noexcept { return impl_->target != nullptr; }
std::uint64_t PhysicalHudOutput::submissions() const noexcept { return impl_->submitted; }
std::int32_t PhysicalHudOutput::last_error() const noexcept { return impl_->error; }
