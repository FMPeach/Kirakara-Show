#pragma once

#include "kirakara/show/config.hpp"
#include "kirakara/show/stage_frame.hpp"
#include "kirakara/show/stage_overlay.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace kirakara::show::win32 {

// Optional CPU submission timings for one Unified Stage render. Values are
// QueryPerformanceCounter ticks and intentionally exclude work that the GPU
// completes asynchronously after the API call returns. ShowHost enables this
// only for the opt-in Phase-0 diagnostics path.
struct UnifiedStageRenderTimings {
    std::uint64_t pool_acquire_qpc{};
    std::uint64_t begin_clear_qpc{};
    std::uint64_t video_composite_qpc{};
    std::uint64_t video_cache_copy_qpc{};
    std::uint64_t title_render_qpc{};
    std::uint64_t lyric_render_qpc{};
    std::uint64_t d2d_flush_qpc{};
    std::uint64_t d3d_flush_qpc{};
    std::uint64_t frame_publish_qpc{};
    std::uint32_t d2d_flush_count{};
    std::uint32_t d3d_flush_count{};
    bool video_cache_refreshed{};
    bool video_cache_reused{};
};

// Stable identity of one decoded video frame. Frame id zero is legal, so
// validity is explicit. Dimensions are part of the identity to prevent a
// format change from reusing a scaled result produced for the old source.
struct UnifiedStageVideoFrameIdentity {
    bool valid{};
    std::uint64_t generation{};
    std::uint64_t frame_id{};
    std::uint32_t width{};
    std::uint32_t height{};
};

struct UnifiedStageRenderRequest {
    StageFrameTiming timing;
    Seconds project_time{};

    // Optional BGRA8 D3D11 texture created on native_d3d_device(). A null
    // texture produces an opaque idle frame without changing playback state.
    void* video_texture{};
    std::uint32_t video_width{};
    std::uint32_t video_height{};
    UnifiedStageVideoFrameIdentity video_identity{};

    const PreparedDocument* lyrics{};
    const AppConfig* config{};
    const StageOverlayState* overlays{};
    bool draw_song_title{true};
    bool draw_lyrics{true};
    Color idle_color{0.0F, 0.0F, 0.0F, 1.0F};
    // Flutter opens shared Stage textures on ANGLE's D3D11 device. Without a
    // native Present boundary, submit producer commands before publishing the
    // handle so ANGLE cannot observe a partially recorded frame.
    bool flush_d3d_before_publish{};
    UnifiedStageRenderTimings* diagnostics{};
};

enum class UnifiedStageRenderResult {
    published,
    dropped_no_free_frame,
    invalid_request,
    render_failed,
};

// Synchronous GPU producer for the common Stage program. It composes one
// immutable frame and publishes it to independent latest-frame mailboxes;
// playback time and scheduling remain owned by ShowHost.
class UnifiedStageRenderer {
public:
    UnifiedStageRenderer();
    ~UnifiedStageRenderer();

    UnifiedStageRenderer(const UnifiedStageRenderer&) = delete;
    UnifiedStageRenderer& operator=(const UnifiedStageRenderer&) = delete;
    UnifiedStageRenderer(UnifiedStageRenderer&&) noexcept;
    UnifiedStageRenderer& operator=(UnifiedStageRenderer&&) noexcept;

    [[nodiscard]] bool configure(const StageOutputProfile& profile,
        std::size_t frame_pool_capacity = StageFramePool::default_capacity,
        void* native_d3d11_device = nullptr);
    [[nodiscard]] bool configured() const noexcept;
    [[nodiscard]] StageOutputProfile profile() const noexcept;
    [[nodiscard]] void* native_d3d_device() noexcept;

    [[nodiscard]] StageFrameMailbox subscribe();
    [[nodiscard]] UnifiedStageRenderResult render(
        const UnifiedStageRenderRequest& request);

    [[nodiscard]] std::uint64_t rendered_frames() const noexcept;
    [[nodiscard]] std::uint64_t dropped_frames() const noexcept;

    // Retires thread-affine producer state while preserving immutable textures
    // still referenced by Sink leases from this renderer generation.
    void retire() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kirakara::show::win32
