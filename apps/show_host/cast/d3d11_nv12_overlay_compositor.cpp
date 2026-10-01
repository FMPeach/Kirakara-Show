#include "d3d11_nv12_overlay_compositor.h"

#include "cast_nv12_surface_pool.h"
#include "cast_pipeline_diagnostics.h"

#include <d3d11.h>

#include "shaders/nv12_overlay_uv_bytecode.h"
#include "shaders/nv12_overlay_y_bytecode.h"

#include <utility>

namespace {

template <typename T>
void release(T*& value) noexcept {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

bool valid_config(
        const D3D11Nv12OverlayCompositorConfig& config) noexcept {
    return config.width != 0 && config.height != 0
        && (config.width & 1U) == 0 && (config.height & 1U) == 0
        && config.surface_count >= 2;
}

bool valid_rect(
        const CastOverlayRect& rect,
        std::uint32_t width,
        std::uint32_t height) noexcept {
    return !rect.empty()
        && (rect.x & 1U) == 0 && (rect.y & 1U) == 0
        && (rect.width & 1U) == 0 && (rect.height & 1U) == 0
        && rect.x <= width && rect.y <= height
        && rect.width <= width - rect.x
        && rect.height <= height - rect.y;
}

struct alignas(16) ComposeRegionConstants {
    std::uint32_t origin_x{};
    std::uint32_t origin_y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

}  // namespace

struct D3D11Nv12OverlayCompositor::Impl {
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    ID3D11ComputeShader* y_shader{};
    ID3D11ComputeShader* uv_shader{};
    ID3D11Buffer* region_constants{};
    ID3D11Texture2D* overlay_texture{};
    ID3D11ShaderResourceView* overlay_srv{};
    CastNv12SurfacePool surfaces;
    D3D11Nv12OverlayCompositorConfig config;
    CastPipelineDiagnostics* diagnostics{};
    D3D11Nv12OverlayFailure failure{D3D11Nv12OverlayFailure::none};
    HRESULT failure_hr{S_OK};
    std::shared_ptr<const D3D11Nv12CompositedFrame> cached;
    void* cached_base_texture{};

    ~Impl() { reset(); }

    void fail(D3D11Nv12OverlayFailure value, HRESULT hr) noexcept {
        failure = value;
        failure_hr = hr;
    }

    void reset() noexcept {
        cached.reset();
        cached_base_texture = nullptr;
        surfaces.reset();
        release(overlay_srv);
        release(overlay_texture);
        release(region_constants);
        release(uv_shader);
        release(y_shader);
        release(context);
        release(device);
        config = {};
        diagnostics = nullptr;
        failure = D3D11Nv12OverlayFailure::none;
        failure_hr = S_OK;
    }

    bool configure(
            ID3D11Device* requested_device,
            const D3D11Nv12OverlayCompositorConfig& requested_config,
            CastPipelineDiagnostics* requested_diagnostics) {
        reset();
        if (!requested_device || !valid_config(requested_config)) {
            fail(D3D11Nv12OverlayFailure::not_configured, E_INVALIDARG);
            return false;
        }
        requested_device->AddRef();
        device = requested_device;
        device->GetImmediateContext(&context);
        if (!context) {
            reset();
            fail(D3D11Nv12OverlayFailure::not_configured, E_FAIL);
            return false;
        }
        auto hr = device->CreateComputeShader(
            kNv12OverlayYShader, sizeof(kNv12OverlayYShader),
            nullptr, &y_shader);
        if (SUCCEEDED(hr)) {
            hr = device->CreateComputeShader(
                kNv12OverlayUvShader, sizeof(kNv12OverlayUvShader),
                nullptr, &uv_shader);
        }
        D3D11_BUFFER_DESC constant_desc{};
        constant_desc.ByteWidth = sizeof(ComposeRegionConstants);
        constant_desc.Usage = D3D11_USAGE_DEFAULT;
        constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (SUCCEEDED(hr)) {
            hr = device->CreateBuffer(
                &constant_desc, nullptr, &region_constants);
        }
        if (FAILED(hr) || !y_shader || !uv_shader || !region_constants) {
            reset();
            fail(D3D11Nv12OverlayFailure::shader_creation_failed, hr);
            return false;
        }

        CastNv12SurfacePoolConfig pool_config;
        pool_config.width = requested_config.width;
        pool_config.height = requested_config.height;
        pool_config.capacity = requested_config.surface_count;
        pool_config.create_plane_srvs = false;
        pool_config.create_plane_uavs = true;
        pool_config.video_processor_output = false;
        pool_config.encoder_compatible = true;
        pool_config.separate_encoder_texture =
            requested_config.separate_encoder_texture;
        if (!surfaces.configure(
                device, pool_config, requested_diagnostics)) {
            reset();
            fail(D3D11Nv12OverlayFailure::not_configured, E_FAIL);
            return false;
        }
        config = requested_config;
        diagnostics = requested_diagnostics;
        fail(D3D11Nv12OverlayFailure::none, S_OK);
        return true;
    }

    bool prepare_overlay_texture(ID3D11Texture2D* requested_texture) {
        if (!device || !context || !requested_texture) {
            fail(D3D11Nv12OverlayFailure::not_configured, E_INVALIDARG);
            return false;
        }
        if (overlay_texture == requested_texture && overlay_srv) return true;

        D3D11_TEXTURE2D_DESC desc{};
        requested_texture->GetDesc(&desc);
        ID3D11Device* texture_device{};
        requested_texture->GetDevice(&texture_device);
        const bool same_device = texture_device == device;
        release(texture_device);
        if (!same_device) {
            fail(D3D11Nv12OverlayFailure::device_mismatch, E_INVALIDARG);
            return false;
        }
        if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM
                || desc.Width != config.width || desc.Height != config.height
                || (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) {
            fail(D3D11Nv12OverlayFailure::invalid_input, E_INVALIDARG);
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC view_desc{};
        view_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view_desc.Texture2D.MipLevels = 1;
        ID3D11ShaderResourceView* candidate{};
        const auto hr = device->CreateShaderResourceView(
            requested_texture, &view_desc, &candidate);
        if (FAILED(hr) || !candidate) {
            release(candidate);
            fail(D3D11Nv12OverlayFailure::invalid_input, hr);
            return false;
        }
        cached.reset();
        cached_base_texture = nullptr;
        release(overlay_srv);
        release(overlay_texture);
        requested_texture->AddRef();
        overlay_texture = requested_texture;
        overlay_srv = candidate;
        if (diagnostics) {
            diagnostics->increment(CastPipelineCounter::view_creations);
        }
        fail(D3D11Nv12OverlayFailure::none, S_OK);
        return true;
    }

    bool validate_base(const D3D11Nv12OverlayInput& base) {
        auto* texture = static_cast<ID3D11Texture2D*>(base.texture);
        if (!texture || !base.y_srv || !base.uv_srv || !base.retention) {
            fail(D3D11Nv12OverlayFailure::invalid_input, E_INVALIDARG);
            return false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        ID3D11Device* texture_device{};
        texture->GetDevice(&texture_device);
        const bool same_device = texture_device == device;
        release(texture_device);
        if (!same_device) {
            fail(D3D11Nv12OverlayFailure::device_mismatch, E_INVALIDARG);
            return false;
        }
        if (desc.Format != DXGI_FORMAT_NV12
                || desc.Width != config.width || desc.Height != config.height) {
            fail(D3D11Nv12OverlayFailure::invalid_input, E_INVALIDARG);
            return false;
        }
        return true;
    }

    bool same_cached_input(
            const D3D11Nv12OverlayInput& base,
            const CastOverlayFrame& overlay) const noexcept {
        return cached && cached->source_frame_id == base.frame_id
            && cached->source_generation == base.generation
            && cached_base_texture == base.texture
            && cached->overlay_content_version == overlay.content_version;
    }

    void unbind_compute_pipeline() noexcept {
        ID3D11ShaderResourceView* null_srvs[3]{};
        ID3D11UnorderedAccessView* null_uavs[2]{};
        ID3D11Buffer* null_buffer{};
        context->CSSetShaderResources(0, 3, null_srvs);
        context->CSSetUnorderedAccessViews(0, 2, null_uavs, nullptr);
        context->CSSetConstantBuffers(0, 1, &null_buffer);
        context->CSSetShader(nullptr, nullptr, 0);
    }

    bool dispatch(
            const D3D11Nv12OverlayInput& base,
            const CastOverlayRect& rect,
            CastNv12SurfacePool::Lease& output) {
        if (!valid_rect(rect, config.width, config.height)) {
            fail(D3D11Nv12OverlayFailure::invalid_input, E_INVALIDARG);
            return false;
        }
        ComposeRegionConstants constants{
            rect.x, rect.y, rect.width, rect.height};
        context->UpdateSubresource(
            region_constants, 0, nullptr, &constants, 0, 0);
        context->CSSetConstantBuffers(0, 1, &region_constants);

        ID3D11ShaderResourceView* y_srvs[3]{
            static_cast<ID3D11ShaderResourceView*>(base.y_srv),
            nullptr,
            overlay_srv};
        auto* y_uav = static_cast<ID3D11UnorderedAccessView*>(output.y_uav());
        context->CSSetShader(y_shader, nullptr, 0);
        context->CSSetShaderResources(0, 3, y_srvs);
        context->CSSetUnorderedAccessViews(0, 1, &y_uav, nullptr);
        context->Dispatch((rect.width + 15U) / 16U,
            (rect.height + 15U) / 16U, 1);
        ID3D11UnorderedAccessView* null_uav{};
        context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
        ID3D11ShaderResourceView* null_srvs[3]{};
        context->CSSetShaderResources(0, 3, null_srvs);

        ID3D11ShaderResourceView* uv_srvs[3]{
            nullptr,
            static_cast<ID3D11ShaderResourceView*>(base.uv_srv),
            overlay_srv};
        auto* uv_uav = static_cast<ID3D11UnorderedAccessView*>(output.uv_uav());
        context->CSSetShader(uv_shader, nullptr, 0);
        context->CSSetShaderResources(0, 3, uv_srvs);
        context->CSSetUnorderedAccessViews(1, 1, &uv_uav, nullptr);
        context->Dispatch((rect.width / 2U + 15U) / 16U,
            (rect.height / 2U + 15U) / 16U, 1);
        unbind_compute_pipeline();
        if (diagnostics) {
            diagnostics->increment(CastPipelineCounter::compute_dispatches, 2);
        }
        return true;
    }

    std::shared_ptr<const D3D11Nv12CompositedFrame> compose(
            const D3D11Nv12OverlayInput& base,
            const CastOverlayFrame& overlay,
            std::uint64_t correlation_id) {
        if (!device || !context) {
            fail(D3D11Nv12OverlayFailure::not_configured, E_UNEXPECTED);
            return {};
        }
        if (!validate_base(base)
                || overlay.width != config.width
                || overlay.height != config.height) {
            if (failure == D3D11Nv12OverlayFailure::none) {
                fail(D3D11Nv12OverlayFailure::invalid_input, E_INVALIDARG);
            }
            return {};
        }
        if (same_cached_input(base, overlay)) {
            fail(D3D11Nv12OverlayFailure::none, S_OK);
            return cached;
        }
        if (overlay.empty && !config.encoder_ready_empty_output) {
            try {
                cached = std::make_shared<D3D11Nv12CompositedFrame>(
                    D3D11Nv12CompositedFrame{
                        base.texture,
                        base.retention,
                        base.frame_id,
                        base.generation,
                        overlay.content_version,
                        true,
                        false});
                cached_base_texture = base.texture;
            } catch (...) {
                fail(D3D11Nv12OverlayFailure::surface_unavailable,
                    E_OUTOFMEMORY);
                return {};
            }
            fail(D3D11Nv12OverlayFailure::none, S_OK);
            return cached;
        }
        if (overlay.empty) {
            auto output = surfaces.try_acquire();
            if (!output) {
                fail(D3D11Nv12OverlayFailure::surface_unavailable,
                    DXGI_ERROR_WAS_STILL_DRAWING);
                return {};
            }
            auto* encoder_texture = static_cast<ID3D11Texture2D*>(
                output.encoder_texture());
            if (!encoder_texture || encoder_texture == base.texture) {
                fail(D3D11Nv12OverlayFailure::invalid_input, E_UNEXPECTED);
                return {};
            }
            const auto started = diagnostics
                ? CastPipelineDiagnostics::qpc_now() : 0;
            context->CopyResource(encoder_texture,
                static_cast<ID3D11Texture2D*>(base.texture));
            if (diagnostics) {
                diagnostics->increment(CastPipelineCounter::gpu_copies);
                diagnostics->record_duration(
                    CastPipelineEvent::gpu_copy,
                    correlation_id,
                    started,
                    CastPipelineDiagnostics::qpc_now(),
                    base.frame_id,
                    static_cast<std::int64_t>(overlay.content_version));
            }
            auto retention = output.retention_token();
            output.reset();
            try {
                cached = std::make_shared<D3D11Nv12CompositedFrame>(
                    D3D11Nv12CompositedFrame{
                        encoder_texture,
                        std::move(retention),
                        base.frame_id,
                        base.generation,
                        overlay.content_version,
                        false,
                        true});
                cached_base_texture = base.texture;
            } catch (...) {
                fail(D3D11Nv12OverlayFailure::surface_unavailable,
                    E_OUTOFMEMORY);
                return {};
            }
            fail(D3D11Nv12OverlayFailure::none, S_OK);
            return cached;
        }
        if (!overlay_texture || !overlay_srv
                || overlay.texture != overlay_texture) {
            fail(D3D11Nv12OverlayFailure::overlay_not_prepared,
                E_INVALIDARG);
            return {};
        }
        auto rect = overlay.changed
            ? overlay.dirty_rect : overlay.coverage_rect;
        if (rect.empty()) rect = overlay.coverage_rect;
        if (!valid_rect(rect, config.width, config.height)) {
            fail(D3D11Nv12OverlayFailure::invalid_input, E_INVALIDARG);
            return {};
        }

        auto output = surfaces.try_acquire();
        if (!output) {
            fail(D3D11Nv12OverlayFailure::surface_unavailable,
                DXGI_ERROR_WAS_STILL_DRAWING);
            return {};
        }
        auto* compose_texture = static_cast<ID3D11Texture2D*>(
            output.compose_texture());
        auto* encoder_texture = static_cast<ID3D11Texture2D*>(
            output.encoder_texture());
        if (!compose_texture || compose_texture == base.texture
                || !output.y_uav() || !output.uv_uav()) {
            fail(D3D11Nv12OverlayFailure::invalid_input, E_UNEXPECTED);
            return {};
        }
        const auto started = diagnostics
            ? CastPipelineDiagnostics::qpc_now() : 0;
        context->CopyResource(compose_texture,
            static_cast<ID3D11Texture2D*>(base.texture));
        if (diagnostics) {
            diagnostics->increment(CastPipelineCounter::gpu_copies);
        }
        if (!dispatch(base, rect, output)) return {};
        const bool encoder_copy = output.needs_encoder_copy();
        if (encoder_copy) {
            context->CopyResource(encoder_texture, compose_texture);
            if (diagnostics) {
                diagnostics->increment(CastPipelineCounter::gpu_copies);
            }
        }

        auto retention = output.retention_token();
        output.reset();
        try {
            cached = std::make_shared<D3D11Nv12CompositedFrame>(
                D3D11Nv12CompositedFrame{
                    encoder_texture,
                    std::move(retention),
                    base.frame_id,
                    base.generation,
                    overlay.content_version,
                    false,
                    encoder_copy});
            cached_base_texture = base.texture;
        } catch (...) {
            fail(D3D11Nv12OverlayFailure::surface_unavailable,
                E_OUTOFMEMORY);
            return {};
        }
        if (diagnostics) {
            diagnostics->record_duration(
                CastPipelineEvent::nv12_compose,
                correlation_id,
                started,
                CastPipelineDiagnostics::qpc_now(),
                base.frame_id,
                static_cast<std::int64_t>(overlay.content_version));
        }
        fail(D3D11Nv12OverlayFailure::none, S_OK);
        return cached;
    }
};

D3D11Nv12OverlayCompositor::D3D11Nv12OverlayCompositor()
    : impl_(std::make_unique<Impl>()) {}

D3D11Nv12OverlayCompositor::~D3D11Nv12OverlayCompositor() = default;

bool D3D11Nv12OverlayCompositor::configure(
        void* d3d11_device,
        const D3D11Nv12OverlayCompositorConfig& config,
        CastPipelineDiagnostics* diagnostics) {
    return impl_ && impl_->configure(
        static_cast<ID3D11Device*>(d3d11_device), config, diagnostics);
}

void D3D11Nv12OverlayCompositor::reset() noexcept {
    if (impl_) impl_->reset();
}

bool D3D11Nv12OverlayCompositor::prepare_overlay_texture(
        void* bgra_texture) {
    return impl_ && impl_->prepare_overlay_texture(
        static_cast<ID3D11Texture2D*>(bgra_texture));
}

std::shared_ptr<const D3D11Nv12CompositedFrame>
D3D11Nv12OverlayCompositor::compose(
        const D3D11Nv12OverlayInput& base,
        const CastOverlayFrame& overlay,
        std::uint64_t correlation_id) {
    return impl_ ? impl_->compose(base, overlay, correlation_id) : nullptr;
}

D3D11Nv12OverlayFailure
D3D11Nv12OverlayCompositor::last_failure() const noexcept {
    return impl_ ? impl_->failure
        : D3D11Nv12OverlayFailure::not_configured;
}

std::int32_t D3D11Nv12OverlayCompositor::last_hresult() const noexcept {
    return impl_ ? static_cast<std::int32_t>(impl_->failure_hr)
        : static_cast<std::int32_t>(E_UNEXPECTED);
}

std::size_t D3D11Nv12OverlayCompositor::available_surfaces() const noexcept {
    return impl_ ? impl_->surfaces.available() : 0;
}
