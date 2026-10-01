#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

class CastPipelineDiagnostics;

struct CastNv12SurfacePoolConfig {
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    std::size_t capacity{8};
    bool create_plane_srvs{};
    bool create_plane_uavs{true};
    bool video_processor_output{};
    bool encoder_compatible{true};
    bool separate_encoder_texture{};
};

// Preallocated NV12 textures and plane views for the Cast pipeline. A lease's
// retention_token can be attached to an asynchronous encoder sample; the slot
// is not reused until both the lease and every copy of that token are gone.
class CastNv12SurfacePool {
public:
    class Lease {
    public:
        Lease() noexcept = default;
        ~Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&&) noexcept = default;
        Lease& operator=(Lease&&) noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] std::size_t slot_index() const noexcept;
        [[nodiscard]] void* compose_texture() const noexcept;
        [[nodiscard]] void* encoder_texture() const noexcept;
        [[nodiscard]] void* y_srv() const noexcept;
        [[nodiscard]] void* uv_srv() const noexcept;
        [[nodiscard]] void* y_uav() const noexcept;
        [[nodiscard]] void* uv_uav() const noexcept;
        [[nodiscard]] bool needs_encoder_copy() const noexcept;

        [[nodiscard]] std::shared_ptr<void> retention_token() const noexcept;
        void reset() noexcept;

    private:
        struct Control;
        explicit Lease(std::shared_ptr<Control> control) noexcept;
        std::shared_ptr<Control> control_;
        friend class CastNv12SurfacePool;
    };

    CastNv12SurfacePool();
    ~CastNv12SurfacePool();

    CastNv12SurfacePool(const CastNv12SurfacePool&) = delete;
    CastNv12SurfacePool& operator=(const CastNv12SurfacePool&) = delete;

    [[nodiscard]] bool configure(
        void* d3d11_device,
        const CastNv12SurfacePoolConfig& config,
        CastPipelineDiagnostics* diagnostics = nullptr);
    void reset() noexcept;

    [[nodiscard]] Lease try_acquire();
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t available() const noexcept;
    [[nodiscard]] void* compose_texture_at(std::size_t index) const noexcept;
    [[nodiscard]] std::uint64_t textures_created() const noexcept;
    [[nodiscard]] std::uint64_t views_created() const noexcept;
    [[nodiscard]] bool retired() const noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};
