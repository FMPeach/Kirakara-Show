#pragma once

#include <cstdint>

// Content key for the last complete Native Stage frame. It intentionally
// describes only inputs that can change pixels. Playback controls continue to
// tick on the host thread; this scheduler only suppresses redundant GPU
// composition/presentation while program time is stationary.
struct NativeStageContentIdentity {
    bool static_content{};
    bool has_video_frame{};
    bool draw_program_overlays{};
    bool native_video_start_pending{};
    bool physical_output{};
    std::int32_t playback_state{};
    std::uint64_t program_generation{};
    std::uint64_t decoded_generation{};
    std::uint64_t decoded_frame_id{};
    std::uint32_t source_width{};
    std::uint32_t source_height{};
    std::int64_t position_100ns{};
    std::uint64_t playback_revision{};
    std::uint64_t overlay_revision{};
    std::uint64_t output_revision{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};

    [[nodiscard]] bool operator==(
        const NativeStageContentIdentity&) const noexcept = default;
};

class NativeStageFrameScheduler {
public:
    [[nodiscard]] bool should_render(
            const NativeStageContentIdentity& identity,
            bool force_redraw = false) const noexcept {
        return force_redraw || !identity.static_content
            || !committed_ || !(identity == committed_identity_);
    }

    void commit(const NativeStageContentIdentity& identity) noexcept {
        committed_identity_ = identity;
        committed_ = true;
    }

    void invalidate() noexcept {
        committed_identity_ = {};
        committed_ = false;
    }

    [[nodiscard]] bool has_committed_frame() const noexcept {
        return committed_;
    }

private:
    NativeStageContentIdentity committed_identity_{};
    bool committed_{};
};
