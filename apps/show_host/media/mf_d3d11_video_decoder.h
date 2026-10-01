#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class CastPipelineDiagnostics;

enum class MfD3D11PlaybackState {
    idle,
    paused,
    playing,
};

enum class MfD3D11VideoPixelFormat : std::uint32_t {
    bgra8,
    nv12,
};

// Decoder-side availability is intentionally separate from playback state.
// In particular, a progressive source that temporarily has no readable sample
// is neither end-of-stream nor a fatal media error. Cast's program clock uses
// this distinction to freeze and resume without advancing audio on its own.
enum class MfD3D11VideoAvailability : std::uint32_t {
    idle,
    opening,
    ready,
    retryable_starvation,
    end_of_stream,
    fatal_error,
};

struct MfD3D11VideoDecoderConfig {
    // Existing Stage callers keep BGRA by default. Native NV12 is opt-in so a
    // partially migrated Cast path cannot accidentally feed NV12 to D2D.
    MfD3D11VideoPixelFormat output_format{MfD3D11VideoPixelFormat::bgra8};
    // Native Stage keeps the historical five-frame queue. Cast may opt into a
    // deeper queue, with an optional byte ceiling for large source textures.
    std::size_t max_queued_frames{5};
    std::size_t max_queued_bytes{};
    double decode_ahead_seconds{0.5};
    // Cast cache files can grow while Source Reader is waiting. Native Stage
    // keeps its historical bounded retry unless this is explicitly enabled.
    bool observe_local_source_growth{};
};

// Values for the colour fields use the Media Foundation enums verbatim. Zero
// means the source did not declare the attribute; policy belongs to the Cast
// normalizer rather than the decoder.
struct MfD3D11VideoFormat {
    MfD3D11VideoPixelFormat pixel_format{MfD3D11VideoPixelFormat::bgra8};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t pixel_aspect_ratio_num{1};
    std::uint32_t pixel_aspect_ratio_den{1};
    std::uint32_t frame_rate_num{};
    std::uint32_t frame_rate_den{};
    std::uint32_t interlace_mode{};
    std::uint32_t rotation_degrees{};
    std::uint32_t video_primaries{};
    std::uint32_t transfer_function{};
    std::uint32_t yuv_matrix{};
    std::uint32_t nominal_range{};
    std::uint32_t chroma_siting{};
    double clean_aperture_x{};
    double clean_aperture_y{};
    std::uint32_t clean_aperture_width{};
    std::uint32_t clean_aperture_height{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return width != 0 && height != 0
            && pixel_aspect_ratio_num != 0 && pixel_aspect_ratio_den != 0;
    }
    [[nodiscard]] constexpr bool has_clean_aperture() const noexcept {
        return clean_aperture_width != 0 && clean_aperture_height != 0;
    }
};

struct MfD3D11VideoBufferStatus {
    std::size_t frame_count{};
    double buffered_duration_seconds{};
    double buffered_start_seconds{};
    double buffered_end_seconds{};
    double source_duration_seconds{};
    bool end_of_stream{};
    MfD3D11VideoAvailability availability{
        MfD3D11VideoAvailability::idle};
    std::int32_t hresult{};
    std::uint64_t format_change_count{};
};

class DecodedD3D11VideoFrame {
public:
    DecodedD3D11VideoFrame(
        void* texture,
        std::shared_ptr<void> texture_lease,
        std::uint32_t width,
        std::uint32_t height,
        double timestamp_seconds,
        double duration_seconds,
        std::uint64_t frame_id = 0,
        std::uint64_t generation = 0,
        MfD3D11VideoFormat format = {}) noexcept;
    ~DecodedD3D11VideoFrame();

    DecodedD3D11VideoFrame(const DecodedD3D11VideoFrame&) = delete;
    DecodedD3D11VideoFrame& operator=(
        const DecodedD3D11VideoFrame&) = delete;

    [[nodiscard]] void* texture() const noexcept;
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] double timestamp_seconds() const noexcept;
    [[nodiscard]] double duration_seconds() const noexcept;
    [[nodiscard]] std::uint64_t frame_id() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] const MfD3D11VideoFormat& format() const noexcept;
    // Keeps a pooled decoder-output slot unavailable for reuse. The texture
    // itself is still COM-owned by this frame; downstream borrowers must keep
    // their own COM reference if they can outlive the frame or decoder pool.
    [[nodiscard]] std::shared_ptr<void> retention_token() const noexcept;

private:
    void* texture_{};
    std::shared_ptr<void> texture_lease_;
    std::uint32_t width_{};
    std::uint32_t height_{};
    double timestamp_seconds_{};
    double duration_seconds_{};
    std::uint64_t frame_id_{};
    std::uint64_t generation_{};
    MfD3D11VideoFormat format_;
};

// Media Foundation decode endpoint for the Cast renderer. PlaybackSession
// synchronizes source/timeline state, then the worker decodes sequentially
// into a small queue of decoder-output D3D11 textures. The renderer consumes
// that queue; it never asks Media Foundation to capture or extract a rendered
// Stage frame. This class never captures a HWND, calls TransferVideoFrame,
// reads pixels back to the CPU, or owns playback time.
class MfD3D11VideoDecoder {
public:
    MfD3D11VideoDecoder();
    ~MfD3D11VideoDecoder();

    MfD3D11VideoDecoder(const MfD3D11VideoDecoder&) = delete;
    MfD3D11VideoDecoder& operator=(const MfD3D11VideoDecoder&) = delete;

    // Exchanges complete worker/session ownership without reopening either
    // Media Foundation pipeline. Both wrappers remain usable after the swap.
    void swap(MfD3D11VideoDecoder& other) noexcept;

    [[nodiscard]] bool start(
        void* d3d11_device,
        CastPipelineDiagnostics* diagnostics = nullptr);
    [[nodiscard]] bool start(
        void* d3d11_device,
        const MfD3D11VideoDecoderConfig& config,
        CastPipelineDiagnostics* diagnostics = nullptr);
    void suspend();
    void stop();

    void synchronize(
        const std::wstring& source,
        std::uint64_t timeline_revision,
        double position_seconds,
        MfD3D11PlaybackState state);

    [[nodiscard]] std::shared_ptr<const DecodedD3D11VideoFrame> texture_for(
        const std::wstring& source,
        std::uint64_t timeline_revision,
        double position_seconds) const;

    [[nodiscard]] MfD3D11VideoBufferStatus buffer_status(
        const std::wstring& source,
        std::uint64_t timeline_revision) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
