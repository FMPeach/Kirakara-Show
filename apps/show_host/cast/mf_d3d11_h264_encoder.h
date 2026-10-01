#pragma once

#include "d3d11_bgra_to_nv12.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

class CastPipelineDiagnostics;

enum class MfH264EncoderPreference {
    automatic,
    hardware_only,
    hardware_readback_only,
    software_only,
};

enum class MfH264EncoderBackend {
    none,
    hardware,
    software,
};

enum class MfH264EncoderInputFormat {
    bgra8,
    nv12,
};

enum class MfH264ForceKeyframeSupport {
    unknown,
    unsupported,
    supported,
    failed,
};

struct MfD3D11H264EncoderConfig {
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    std::uint32_t frame_rate_num{60};
    std::uint32_t frame_rate_den{1};
    std::uint32_t bitrate{6000000};
    std::uint32_t gop_size_frames{60};
    MfH264EncoderPreference preference{MfH264EncoderPreference::automatic};
    // Fixed for the lifetime of one encoder session. The default preserves
    // the existing Stage BGRA conversion path.
    MfH264EncoderInputFormat input_format{MfH264EncoderInputFormat::bgra8};
};

// Native H.264 encoder for the fixed-canvas Cast Stage texture. Hardware MFTs
// consume GPU NV12 textures directly by default. The explicit hardware
// readback mode is a compatibility fallback for drivers that reject DXGI
// samples; the software fallback uses the same bounded NV12 readback. No path
// captures an HWND.
class MfD3D11H264Encoder {
public:
    // `data` is a borrowed view of the MFT output buffer and remains valid
    // only for the duration of the callback. Consumers must finish reading or
    // copy it before returning; the production Cast muxer consumes it
    // synchronously and therefore avoids an intermediate access-unit copy.
    using OutputCallback = std::function<void(
        const std::uint8_t* data,
        std::size_t size,
        std::int64_t pts90k,
        bool keyframe)>;

    MfD3D11H264Encoder();
    ~MfD3D11H264Encoder();

    MfD3D11H264Encoder(const MfD3D11H264Encoder&) = delete;
    MfD3D11H264Encoder& operator=(const MfD3D11H264Encoder&) = delete;

    [[nodiscard]] bool start(
        void* d3d11_device,
        const MfD3D11H264EncoderConfig& config,
        OutputCallback output,
        CastPipelineDiagnostics* diagnostics = nullptr,
        void* startup_nv12_texture = nullptr,
        std::shared_ptr<void> startup_retention = {});
    void stop();

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] MfH264EncoderBackend backend() const noexcept;
    [[nodiscard]] MfH264ForceKeyframeSupport force_keyframe_support()
        const noexcept;
    [[nodiscard]] bool startup_sample_retained_async() const noexcept;

    // Precreate the video-processor input view for the fixed Stage texture.
    // The host calls this during Cast startup so the per-frame path performs
    // no D3D11 resource/view creation.
    [[nodiscard]] bool prepare_input_texture(void* composed_bgra_texture);

    // sample_time_100ns belongs to the continuous Cast transport clock, not
    // the song playback clock. Returns false when the bounded encoder queue is
    // full so the caller can drop the output frame without blocking Stage.
    [[nodiscard]] bool submit_texture(
        void* composed_bgra_texture,
        std::int64_t sample_time_100ns,
        std::int64_t sample_duration_100ns,
        bool force_keyframe = false);

    // Submit another transport sample for an unchanged Stage texture. The
    // encoder reuses the last matching BGRA->NV12 conversion, avoiding one
    // video-processor pass per held frame. If no matching conversion is
    // cached yet, this transparently performs a normal submission.
    [[nodiscard]] bool submit_repeated_texture(
        void* unchanged_bgra_texture,
        std::int64_t sample_time_100ns,
        std::int64_t sample_duration_100ns,
        bool force_keyframe = false);

    // Submit an immutable NV12 texture owned by the caller's retention token.
    // The token remains alive through the asynchronous MFT sample callback (or
    // software readback), so pooled textures cannot be reused too early.
    [[nodiscard]] bool submit_nv12_texture(
        void* nv12_texture,
        std::shared_ptr<void> retention,
        std::int64_t sample_time_100ns,
        std::int64_t sample_duration_100ns,
        bool force_keyframe = false);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
