#include "kirakara/show/win32/stage_frame_resource.hpp"

#include <d3d11.h>
#include <dxgi.h>

namespace kirakara::show::win32 {

std::shared_ptr<D3D11StageFrameResource> D3D11StageFrameResource::create(
        const StageOutputProfile& profile, void* native_d3d11_device) {
    if (!profile.valid() || profile.pixel_format != StagePixelFormat::bgra8
            || !native_d3d11_device) {
        return {};
    }
    auto resource = std::shared_ptr<D3D11StageFrameResource>(
        new D3D11StageFrameResource());
    if (resource->surface_.resize_shared_on_device(native_d3d11_device,
            profile.width, profile.height)) {
        auto* texture = static_cast<ID3D11Texture2D*>(
            resource->surface_.native_texture());
        IDXGIResource* shared_resource{};
        HANDLE shared_handle{};
        if (texture && SUCCEEDED(texture->QueryInterface(
                IID_PPV_ARGS(&shared_resource)))) {
            if (FAILED(shared_resource->GetSharedHandle(&shared_handle))) {
                shared_handle = nullptr;
            }
            shared_resource->Release();
        }
        resource->shared_handle_ = reinterpret_cast<std::uintptr_t>(
            shared_handle);
    }
    if (resource->shared_handle_ == 0
            && !resource->surface_.resize_on_device(native_d3d11_device,
                profile.width, profile.height)) {
        return {};
    }
    return resource;
}

std::shared_ptr<D3D11StageFrameResource> D3D11StageFrameResource::create(
        const StageOutputProfile& profile,
        const D3D11TextureSurface& resource_domain) {
    if (!profile.valid() || profile.pixel_format != StagePixelFormat::bgra8
            || !resource_domain.native_texture()) {
        return {};
    }
    auto resource = std::shared_ptr<D3D11StageFrameResource>(
        new D3D11StageFrameResource());
    if (!resource->surface_.resize_shared_in_resource_domain(
            resource_domain, profile.width, profile.height)) {
        return {};
    }
    auto* texture = static_cast<ID3D11Texture2D*>(
        resource->surface_.native_texture());
    IDXGIResource* shared_resource{};
    HANDLE shared_handle{};
    if (texture && SUCCEEDED(texture->QueryInterface(
            IID_PPV_ARGS(&shared_resource)))) {
        if (FAILED(shared_resource->GetSharedHandle(&shared_handle))) {
            shared_handle = nullptr;
        }
        shared_resource->Release();
    }
    resource->shared_handle_ = reinterpret_cast<std::uintptr_t>(shared_handle);
    return resource->shared_handle_ != 0 ? resource : nullptr;
}

void* D3D11StageFrameResource::native_resource() noexcept {
    return surface_.native_texture();
}

std::uintptr_t D3D11StageFrameResource::shared_handle() const noexcept {
    return shared_handle_;
}

void D3D11StageFrameResource::retire_producer_access() noexcept {
    surface_.retire_producer_access();
}

D3D11TextureSurface& D3D11StageFrameResource::surface() noexcept {
    return surface_;
}

const D3D11TextureSurface& D3D11StageFrameResource::surface() const noexcept {
    return surface_;
}

} // namespace kirakara::show::win32
