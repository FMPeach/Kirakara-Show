#pragma once

#include "cast_nv12_surface_pool.h"
#include "../media/mf_d3d11_video_decoder.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

class CastPipelineDiagnostics;

struct CastVideoFrameNormalizerConfig {
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    std::uint32_t frame_rate_num{60};
    std::uint32_t frame_rate_den{1};
    std::size_t surface_count{4};
    bool create_plane_srvs{true};
    // Exact-size SDR decoder frames may be borrowed directly. This is opt-in
    // so existing library callers retain the historical copy semantics.
    bool allow_direct_passthrough{};
};

struct CastVideoFrameInput {
    void* texture{};
    MfD3D11VideoFormat format;
    double timestamp_seconds{};
    double duration_seconds{};
    std::uint64_t frame_id{};
    std::uint64_t generation{};
    std::shared_ptr<void> retention;
};

struct CastVideoNormalizeRect {
    std::int32_t left{};
    std::int32_t top{};
    std::int32_t right{};
    std::int32_t bottom{};

    [[nodiscard]] constexpr std::int32_t width() const noexcept {
        return right - left;
    }
    [[nodiscard]] constexpr std::int32_t height() const noexcept {
        return bottom - top;
    }
};

struct CastVideoNormalizeGeometry {
    CastVideoNormalizeRect source;
    CastVideoNormalizeRect destination;
    std::uint32_t rotation_degrees{};
    bool direct_copy{};
};

// Pure geometry policy shared by the D3D11 implementation and tests. Source
// and destination edges are always even so NV12 chroma samples are not split.
[[nodiscard]] std::optional<CastVideoNormalizeGeometry>
cast_calculate_video_normalize_geometry(
    const MfD3D11VideoFormat& format,
    std::uint32_t output_width,
    std::uint32_t output_height) noexcept;

enum class CastVideoNormalizeFailure : std::uint32_t {
    none,
    not_configured,
    invalid_frame,
    device_mismatch,
    hdr_unsupported,
    rotation_unsupported,
    surface_unavailable,
    plane_views_unavailable,
    video_processor_unavailable,
    video_processor_failed,
};

class CastNormalizedVideoFrame {
public:
    CastNormalizedVideoFrame(
        CastNv12SurfacePool::Lease lease,
        MfD3D11VideoFormat format,
        double timestamp_seconds,
        double duration_seconds,
        std::uint64_t frame_id,
        std::uint64_t generation) noexcept;
    CastNormalizedVideoFrame(
        void* texture,
        void* y_srv,
        void* uv_srv,
        std::shared_ptr<void> retention,
        MfD3D11VideoFormat format,
        double timestamp_seconds,
        double duration_seconds,
        std::uint64_t frame_id,
        std::uint64_t generation) noexcept;
    ~CastNormalizedVideoFrame();

    CastNormalizedVideoFrame(const CastNormalizedVideoFrame&) = delete;
    CastNormalizedVideoFrame& operator=(
        const CastNormalizedVideoFrame&) = delete;

    [[nodiscard]] void* texture() const noexcept;
    [[nodiscard]] void* y_srv() const noexcept;
    [[nodiscard]] void* uv_srv() const noexcept;
    [[nodiscard]] std::shared_ptr<void> retention_token() const noexcept;
    [[nodiscard]] const MfD3D11VideoFormat& format() const noexcept;
    [[nodiscard]] double timestamp_seconds() const noexcept;
    [[nodiscard]] double duration_seconds() const noexcept;
    [[nodiscard]] std::uint64_t frame_id() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;

private:
    CastNv12SurfacePool::Lease lease_;
    void* borrowed_texture_{};
    void* borrowed_y_srv_{};
    void* borrowed_uv_srv_{};
    std::shared_ptr<void> borrowed_retention_;
    MfD3D11VideoFormat format_;
    double timestamp_seconds_{};
    double duration_seconds_{};
    std::uint64_t frame_id_{};
    std::uint64_t generation_{};
};

// Cast-only source-frame cache. It never owns a decoder or advances time: one
// new source-frame identity produces at most one GPU normalize operation.
class CastVideoFrameNormalizer {
public:
    CastVideoFrameNormalizer();
    ~CastVideoFrameNormalizer();

    CastVideoFrameNormalizer(const CastVideoFrameNormalizer&) = delete;
    CastVideoFrameNormalizer& operator=(
        const CastVideoFrameNormalizer&) = delete;

    [[nodiscard]] bool configure(
        void* d3d11_device,
        const CastVideoFrameNormalizerConfig& config = {},
        CastPipelineDiagnostics* diagnostics = nullptr);
    void reset() noexcept;

    [[nodiscard]] std::shared_ptr<const CastNormalizedVideoFrame> normalize(
        const CastVideoFrameInput& input);
    [[nodiscard]] CastVideoNormalizeFailure last_failure() const noexcept;
    [[nodiscard]] std::int32_t last_hresult() const noexcept;
    [[nodiscard]] std::size_t available_surfaces() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
