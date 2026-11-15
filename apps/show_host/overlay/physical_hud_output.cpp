#include "physical_hud_output.h"
#include "host_hud_renderer.h"

#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cmath>
#include <utility>

using Microsoft::WRL::ComPtr;

struct PhysicalHudOutput::Impl {
    struct Layer {
        ComPtr<IDCompositionVisual> visual;
        ComPtr<IDCompositionEffectGroup> opacity_effect;
        ComPtr<IDXGISwapChain1> swap_chain;
        ComPtr<ID3D11Texture2D> source;
        std::uint32_t width{}, height{}, x{}, y{};
        std::uint64_t revision{};
        float opacity{1};
        bool visible{}, has_content{}, effect_attached{};
    };

    HWND window{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDCompositionDevice> composition;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> root;
    Layer main, action;
    std::uint64_t submitted{};
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
        main = {};
        action = {};
        root.Reset();
        target.Reset();
        composition.Reset();
        factory.Reset();
        context.Reset();
        device.Reset();
        window = nullptr;
        submitted = 0;
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
                || !check(composition->CreateVisual(&root))
                || !check(composition->CreateVisual(&main.visual))
                || !check(target->SetRoot(root.Get()))
                || !check(composition->Commit())) {
            shutdown();
            return false;
        }
        return true;
    }

    bool update(Layer& layer, const HostHudFrame& frame, bool& dirty) {
        if (!frame.visible) {
            if (layer.visible) {
                if (!check(root->RemoveVisual(layer.visual.Get()))) return false;
                layer.visible = false;
                dirty = true;
            }
            return true;
        }

        auto* texture = static_cast<ID3D11Texture2D*>(frame.texture);
        if (!texture || !std::isfinite(frame.opacity) || frame.opacity < 0 || frame.opacity > 1) {
            return check(E_INVALIDARG);
        }
        if (layer.visible && layer.has_content && layer.source.Get() == texture
                && layer.revision == frame.revision && layer.x == frame.x
                && layer.y == frame.y && layer.opacity == frame.opacity) return true;
        if (!layer.visual && !check(composition->CreateVisual(&layer.visual))) return false;
        if (!layer.has_content || layer.source.Get() != texture || layer.revision != frame.revision) {
            ComPtr<ID3D11Device> input_device;
            texture->GetDevice(&input_device);
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            if (input_device.Get() != device.Get()
                    || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM
                    || desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.ArraySize != 1) {
                return check(E_INVALIDARG);
            }
            const bool resize = !layer.swap_chain
                || layer.width != desc.Width || layer.height != desc.Height;
            auto* swap_chain = layer.swap_chain.Get();
            ComPtr<IDXGISwapChain1> replacement;
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
                if (!check(factory->CreateSwapChainForComposition(device.Get(),
                            &config, nullptr, &replacement))) return false;
                swap_chain = replacement.Get();
            }
            ComPtr<ID3D11Texture2D> buffer;
            if (!check(swap_chain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return false;
            context->CopyResource(buffer.Get(), texture);
            if (!check(swap_chain->Present(0, 0))) return false;
            ++submitted;
            if (resize) {
                if (!check(layer.visual->SetContent(replacement.Get()))) return false;
                layer.swap_chain = std::move(replacement);
                layer.width = desc.Width;
                layer.height = desc.Height;
                dirty = true;
            }
            layer.source = texture;
            layer.revision = frame.revision;
            layer.has_content = true;
        }

        if (layer.opacity != frame.opacity) {
            if (frame.opacity < 1) {
                if (!layer.opacity_effect
                        && !check(composition->CreateEffectGroup(&layer.opacity_effect))) return false;
                if (!check(layer.opacity_effect->SetOpacity(frame.opacity))) return false;
                if (!layer.effect_attached
                        && !check(layer.visual->SetEffect(layer.opacity_effect.Get()))) return false;
                layer.effect_attached = true;
            } else if (layer.effect_attached) {
                if (!check(layer.visual->SetEffect(nullptr))) return false;
                layer.effect_attached = false;
            }
            layer.opacity = frame.opacity;
            dirty = true;
        }
        if (layer.x != frame.x || layer.y != frame.y || !layer.visible) {
            if (!check(layer.visual->SetOffsetX(static_cast<float>(frame.x)))
                    || !check(layer.visual->SetOffsetY(static_cast<float>(frame.y)))) return false;
            layer.x = frame.x;
            layer.y = frame.y;
            dirty = true;
        }
        if (!layer.visible) {
            const auto* reference = &layer == &action ? &main : &action;
            // The action sits above the main card if their edges ever meet.
            if (!check(root->AddVisual(layer.visual.Get(), &layer == &action,
                    reference->visible ? reference->visual.Get() : nullptr))) return false;
            layer.visible = true;
            dirty = true;
        }
        return true;
    }

    bool present(const HostHudLayers& layers) {
        if (!target) return !layers.card.visible && !layers.action.visible;
        bool dirty{};
        if (!update(main, layers.card, dirty) || !update(action, layers.action, dirty)
                || (dirty && !check(composition->Commit()))) {
            // Recover both layers together instead of committing half a layout.
            shutdown();
            return false;
        }
        return check(S_OK);
    }
};

PhysicalHudOutput::PhysicalHudOutput() : impl_(std::make_unique<Impl>()) {}
PhysicalHudOutput::~PhysicalHudOutput() { impl_->shutdown(); }
bool PhysicalHudOutput::initialize(void* hwnd, void* device) {
    return impl_->initialize(static_cast<HWND>(hwnd), static_cast<ID3D11Device*>(device));
}
bool PhysicalHudOutput::present(const HostHudLayers& layers) {
    return impl_->present(layers);
}
void PhysicalHudOutput::shutdown() noexcept { impl_->shutdown(); }
bool PhysicalHudOutput::initialized() const noexcept { return impl_->target != nullptr; }
std::uint64_t PhysicalHudOutput::submissions() const noexcept { return impl_->submitted; }
std::int32_t PhysicalHudOutput::last_error() const noexcept { return impl_->error; }
