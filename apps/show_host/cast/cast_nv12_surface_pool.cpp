#include "cast_nv12_surface_pool.h"

#include "cast_pipeline_diagnostics.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#include <d3d11.h>

namespace {

template <typename T>
void release(T*& value) noexcept {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

bool valid_config(const CastNv12SurfacePoolConfig& config) noexcept {
    return config.width != 0 && config.height != 0
        && (config.width & 1U) == 0 && (config.height & 1U) == 0
        && config.capacity != 0
        && config.capacity <= static_cast<std::size_t>(
            std::numeric_limits<UINT>::max())
        && (config.create_plane_srvs || config.create_plane_uavs
            || config.video_processor_output || config.encoder_compatible)
        && (!config.separate_encoder_texture || config.encoder_compatible);
}

}  // namespace

struct CastNv12SurfacePool::State {
    struct Slot {
        ID3D11Texture2D* compose_texture{};
        ID3D11Texture2D* encoder_texture{};
        ID3D11ShaderResourceView* y_srv{};
        ID3D11ShaderResourceView* uv_srv{};
        ID3D11UnorderedAccessView* y_uav{};
        ID3D11UnorderedAccessView* uv_uav{};
        bool in_use{};

        Slot() = default;
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
        Slot(Slot&& other) noexcept
            : compose_texture(std::exchange(other.compose_texture, nullptr)),
              encoder_texture(std::exchange(other.encoder_texture, nullptr)),
              y_srv(std::exchange(other.y_srv, nullptr)),
              uv_srv(std::exchange(other.uv_srv, nullptr)),
              y_uav(std::exchange(other.y_uav, nullptr)),
              uv_uav(std::exchange(other.uv_uav, nullptr)),
              in_use(other.in_use) {}
        Slot& operator=(Slot&& other) noexcept {
            if (this == &other) return *this;
            clear();
            compose_texture = std::exchange(other.compose_texture, nullptr);
            encoder_texture = std::exchange(other.encoder_texture, nullptr);
            y_srv = std::exchange(other.y_srv, nullptr);
            uv_srv = std::exchange(other.uv_srv, nullptr);
            y_uav = std::exchange(other.y_uav, nullptr);
            uv_uav = std::exchange(other.uv_uav, nullptr);
            in_use = other.in_use;
            return *this;
        }
        ~Slot() { clear(); }

        void clear() noexcept {
            release(uv_uav);
            release(y_uav);
            release(uv_srv);
            release(y_srv);
            if (encoder_texture != compose_texture) {
                release(encoder_texture);
            } else {
                encoder_texture = nullptr;
            }
            release(compose_texture);
        }
    };

    ~State() {
        slots.clear();
        release(device);
    }

    mutable std::mutex mutex;
    ID3D11Device* device{};
    CastNv12SurfacePoolConfig config;
    std::vector<Slot> slots;
    std::uint64_t textures_created{};
    std::uint64_t views_created{};
    bool retired{};
};

struct CastNv12SurfacePool::Lease::Control {
    Control(std::shared_ptr<State> owner, std::size_t slot) noexcept
        : state(std::move(owner)), slot_index(slot) {}

    ~Control() {
        if (!state) return;
        std::lock_guard lock(state->mutex);
        if (slot_index < state->slots.size()) {
            state->slots[slot_index].in_use = false;
        }
    }

    std::shared_ptr<State> state;
    std::size_t slot_index{};
};

CastNv12SurfacePool::Lease::Lease(
        std::shared_ptr<Control> control) noexcept
    : control_(std::move(control)) {}

CastNv12SurfacePool::Lease::operator bool() const noexcept {
    return control_ && control_->state
        && control_->slot_index < control_->state->slots.size();
}

std::size_t CastNv12SurfacePool::Lease::slot_index() const noexcept {
    return control_ ? control_->slot_index : static_cast<std::size_t>(-1);
}

void* CastNv12SurfacePool::Lease::compose_texture() const noexcept {
    return *this
        ? control_->state->slots[control_->slot_index].compose_texture
        : nullptr;
}

void* CastNv12SurfacePool::Lease::encoder_texture() const noexcept {
    return *this
        ? control_->state->slots[control_->slot_index].encoder_texture
        : nullptr;
}

void* CastNv12SurfacePool::Lease::y_srv() const noexcept {
    return *this ? control_->state->slots[control_->slot_index].y_srv : nullptr;
}

void* CastNv12SurfacePool::Lease::uv_srv() const noexcept {
    return *this ? control_->state->slots[control_->slot_index].uv_srv : nullptr;
}

void* CastNv12SurfacePool::Lease::y_uav() const noexcept {
    return *this ? control_->state->slots[control_->slot_index].y_uav : nullptr;
}

void* CastNv12SurfacePool::Lease::uv_uav() const noexcept {
    return *this ? control_->state->slots[control_->slot_index].uv_uav : nullptr;
}

bool CastNv12SurfacePool::Lease::needs_encoder_copy() const noexcept {
    return *this && control_->state->config.separate_encoder_texture;
}

std::shared_ptr<void>
CastNv12SurfacePool::Lease::retention_token() const noexcept {
    return control_;
}

void CastNv12SurfacePool::Lease::reset() noexcept {
    control_.reset();
}

CastNv12SurfacePool::CastNv12SurfacePool() = default;
CastNv12SurfacePool::~CastNv12SurfacePool() { reset(); }

bool CastNv12SurfacePool::configure(
        void* d3d11_device,
        const CastNv12SurfacePoolConfig& config,
        CastPipelineDiagnostics* diagnostics) {
    auto* device = static_cast<ID3D11Device*>(d3d11_device);
    if (!device || !valid_config(config)) return false;

    auto candidate = std::make_shared<State>();
    device->AddRef();
    candidate->device = device;
    candidate->config = config;
    candidate->slots.reserve(config.capacity);

    UINT compose_bind_flags{};
    if (config.create_plane_srvs) {
        compose_bind_flags |= D3D11_BIND_SHADER_RESOURCE;
    }
    if (config.create_plane_uavs) {
        compose_bind_flags |= D3D11_BIND_UNORDERED_ACCESS;
    }
    if (config.video_processor_output) {
        compose_bind_flags |= D3D11_BIND_RENDER_TARGET;
    }
    if (config.encoder_compatible && !config.separate_encoder_texture) {
        compose_bind_flags |= D3D11_BIND_VIDEO_ENCODER;
    }

    for (std::size_t index = 0; index < config.capacity; ++index) {
        State::Slot slot;
        D3D11_TEXTURE2D_DESC texture_desc{};
        texture_desc.Width = config.width;
        texture_desc.Height = config.height;
        texture_desc.MipLevels = 1;
        texture_desc.ArraySize = 1;
        texture_desc.Format = DXGI_FORMAT_NV12;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.Usage = D3D11_USAGE_DEFAULT;
        texture_desc.BindFlags = compose_bind_flags;
        auto hr = device->CreateTexture2D(
            &texture_desc, nullptr, &slot.compose_texture);
        if (FAILED(hr) || !slot.compose_texture) return false;
        ++candidate->textures_created;
        if (diagnostics) {
            diagnostics->increment(CastPipelineCounter::texture_creations);
        }

        if (config.separate_encoder_texture) {
            texture_desc.BindFlags = D3D11_BIND_VIDEO_ENCODER;
            hr = device->CreateTexture2D(
                &texture_desc, nullptr, &slot.encoder_texture);
            if (FAILED(hr) || !slot.encoder_texture) return false;
            ++candidate->textures_created;
            if (diagnostics) {
                diagnostics->increment(
                    CastPipelineCounter::texture_creations);
            }
        } else {
            slot.encoder_texture = slot.compose_texture;
        }

        if (config.create_plane_srvs) {
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};
            view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = 1;
            view.Format = DXGI_FORMAT_R8_UNORM;
            hr = device->CreateShaderResourceView(
                slot.compose_texture, &view, &slot.y_srv);
            if (FAILED(hr) || !slot.y_srv) return false;
            ++candidate->views_created;
            if (diagnostics) {
                diagnostics->increment(CastPipelineCounter::view_creations);
            }
            view.Format = DXGI_FORMAT_R8G8_UNORM;
            hr = device->CreateShaderResourceView(
                slot.compose_texture, &view, &slot.uv_srv);
            if (FAILED(hr) || !slot.uv_srv) return false;
            ++candidate->views_created;
            if (diagnostics) {
                diagnostics->increment(CastPipelineCounter::view_creations);
            }
        }

        if (config.create_plane_uavs) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
            view.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            view.Format = DXGI_FORMAT_R8_UNORM;
            hr = device->CreateUnorderedAccessView(
                slot.compose_texture, &view, &slot.y_uav);
            if (FAILED(hr) || !slot.y_uav) return false;
            ++candidate->views_created;
            if (diagnostics) {
                diagnostics->increment(CastPipelineCounter::view_creations);
            }
            view.Format = DXGI_FORMAT_R8G8_UNORM;
            hr = device->CreateUnorderedAccessView(
                slot.compose_texture, &view, &slot.uv_uav);
            if (FAILED(hr) || !slot.uv_uav) return false;
            ++candidate->views_created;
            if (diagnostics) {
                diagnostics->increment(CastPipelineCounter::view_creations);
            }
        }
        candidate->slots.push_back(std::move(slot));
    }

    reset();
    state_ = std::move(candidate);
    return true;
}

void CastNv12SurfacePool::reset() noexcept {
    if (!state_) return;
    {
        std::lock_guard lock(state_->mutex);
        state_->retired = true;
    }
    state_.reset();
}

CastNv12SurfacePool::Lease CastNv12SurfacePool::try_acquire() {
    auto state = state_;
    if (!state) return {};
    std::lock_guard lock(state->mutex);
    if (state->retired) return {};
    for (std::size_t index = 0; index < state->slots.size(); ++index) {
        if (state->slots[index].in_use) continue;
        state->slots[index].in_use = true;
        try {
            return Lease(std::make_shared<Lease::Control>(state, index));
        } catch (...) {
            state->slots[index].in_use = false;
            return {};
        }
    }
    return {};
}

std::size_t CastNv12SurfacePool::capacity() const noexcept {
    auto state = state_;
    if (!state) return 0;
    std::lock_guard lock(state->mutex);
    return state->slots.size();
}

std::size_t CastNv12SurfacePool::available() const noexcept {
    auto state = state_;
    if (!state) return 0;
    std::lock_guard lock(state->mutex);
    if (state->retired) return 0;
    return static_cast<std::size_t>(std::count_if(
        state->slots.begin(), state->slots.end(),
        [](const State::Slot& slot) { return !slot.in_use; }));
}

void* CastNv12SurfacePool::compose_texture_at(
        std::size_t index) const noexcept {
    auto state = state_;
    if (!state) return nullptr;
    std::lock_guard lock(state->mutex);
    return !state->retired && index < state->slots.size()
        ? state->slots[index].compose_texture : nullptr;
}

std::uint64_t CastNv12SurfacePool::textures_created() const noexcept {
    auto state = state_;
    if (!state) return 0;
    std::lock_guard lock(state->mutex);
    return state->textures_created;
}

std::uint64_t CastNv12SurfacePool::views_created() const noexcept {
    auto state = state_;
    if (!state) return 0;
    std::lock_guard lock(state->mutex);
    return state->views_created;
}

bool CastNv12SurfacePool::retired() const noexcept {
    auto state = state_;
    if (!state) return true;
    std::lock_guard lock(state->mutex);
    return state->retired;
}
