#include "mf_d3d11_video_decoder.h"
#include "ffmpeg_decode.h"

#include "../cast/cast_pipeline_diagnostics.h"
#include "../host/host_utils.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <d3d11.h>
#include <mfobjects.h>
#include <objbase.h>

namespace {

constexpr double kTimestampToleranceSeconds = 1.0 / 240.0;
constexpr double kDefaultFrameDurationSeconds = 1.0 / 30.0;
constexpr double kMaximumContinuityGapSeconds = 0.100;
// Compare consecutive requested playback positions, not decoder output. A
// decoder that falls behind still receives a smooth request timeline and must
// not seek repeatedly, while a hidden output resumes with one large jump.
constexpr double kPlaybackPositionDiscontinuitySeconds = 0.25;
constexpr std::size_t kMaximumConfiguredQueueFrames = 64;
constexpr double kMaximumConfiguredDecodeAheadSeconds = 2.0;
constexpr auto kLocalGrowthFallbackRetry = std::chrono::seconds(1);
constexpr auto kDefaultSourceRetry = std::chrono::milliseconds(100);

MfD3D11VideoDecoderConfig sanitize_decoder_config(
        MfD3D11VideoDecoderConfig config) noexcept {
    config.max_queued_frames = std::clamp<std::size_t>(
        config.max_queued_frames, 1, kMaximumConfiguredQueueFrames);
    if (!std::isfinite(config.decode_ahead_seconds)
            || config.decode_ahead_seconds < 0.0) {
        config.decode_ahead_seconds = 0.0;
    }
    config.decode_ahead_seconds = std::min(
        config.decode_ahead_seconds,
        kMaximumConfiguredDecodeAheadSeconds);
    return config;
}

template <typename T>
void release(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

double sanitize_position(double seconds) {
    return std::isfinite(seconds) && seconds > 0.0 ? seconds : 0.0;
}

struct PlaybackDirective {
    std::wstring source;
    std::uint64_t timeline_revision{};
    double position_seconds{};
    MfD3D11PlaybackState state{MfD3D11PlaybackState::idle};
    std::uint64_t serial{};
    std::uint64_t seek_epoch{};
};

struct DecodedTextureQueue {
    std::wstring source;
    std::uint64_t timeline_revision{};
    std::uint64_t seek_epoch{};
    std::deque<std::shared_ptr<const DecodedD3D11VideoFrame>> textures;
    bool end_of_stream{};
    MfD3D11VideoAvailability availability{
        MfD3D11VideoAvailability::idle};
    HRESULT hresult{S_OK};
    double source_duration_seconds{};
    std::uint64_t format_change_count{};
};

std::size_t estimated_frame_bytes(
        const DecodedD3D11VideoFrame& frame) noexcept {
    const auto pixels = static_cast<std::uint64_t>(frame.width())
        * static_cast<std::uint64_t>(frame.height());
    const auto bytes = frame.format().pixel_format
            == MfD3D11VideoPixelFormat::nv12
        ? pixels + pixels / 2U
        : pixels * 4U;
    return bytes > std::numeric_limits<std::size_t>::max()
        ? std::numeric_limits<std::size_t>::max()
        : static_cast<std::size_t>(bytes);
}

std::size_t effective_queue_frame_limit(
        const DecodedTextureQueue& queue,
        const MfD3D11VideoDecoderConfig& config) noexcept {
    auto limit = config.max_queued_frames;
    if (config.max_queued_bytes == 0 || queue.textures.empty()) return limit;
    const auto bytes = estimated_frame_bytes(*queue.textures.front());
    if (bytes == 0) return limit;
    const auto memory_limited = std::max<std::size_t>(
        1, config.max_queued_bytes / bytes);
    return std::min(limit, memory_limited);
}

double frame_end_seconds(
        const DecodedD3D11VideoFrame& frame) noexcept {
    return frame.timestamp_seconds()
        + std::max(frame.duration_seconds(), 0.0);
}

double contiguous_buffer_end_seconds(
        const DecodedTextureQueue& queue) noexcept {
    if (queue.textures.empty()) return 0.0;
    auto end = frame_end_seconds(*queue.textures.front());
    auto previous_duration = std::max(
        queue.textures.front()->duration_seconds(),
        kDefaultFrameDurationSeconds);
    for (std::size_t index = 1; index < queue.textures.size(); ++index) {
        const auto& frame = *queue.textures[index];
        const auto duration = std::max(
            frame.duration_seconds(), kDefaultFrameDurationSeconds);
        const auto tolerance = std::clamp(
            std::max(previous_duration, duration) * 3.0,
            kTimestampToleranceSeconds,
            kMaximumContinuityGapSeconds);
        if (frame.timestamp_seconds() > end + tolerance) break;
        end = std::max(end, frame_end_seconds(frame));
        previous_duration = duration;
    }
    return end;
}

class LocalSourceGrowthNotification {
public:
    LocalSourceGrowthNotification() = default;
    ~LocalSourceGrowthNotification() { reset(); }

    LocalSourceGrowthNotification(const LocalSourceGrowthNotification&) =
        delete;
    LocalSourceGrowthNotification& operator=(
        const LocalSourceGrowthNotification&) = delete;

    void observe(const std::wstring& source) {
        // Keep a live watcher for an unchanged source, but retry creating it
        // when the directory was temporarily unavailable on the previous
        // attempt (for example while a cache generation is being published).
        if (source == source_ && active()) return;
        if (source == source_ && (source.empty()
                || source.starts_with(L"http://")
                || source.starts_with(L"https://")
                || source.starts_with(L"file://"))) {
            return;
        }
        reset();
        source_ = source;
        if (source.empty() || source.starts_with(L"http://")
                || source.starts_with(L"https://")
                || source.starts_with(L"file://")) {
            return;
        }

        std::error_code error;
        auto path = std::filesystem::absolute(
            std::filesystem::path(source), error);
        if (error || path.empty()) return;
        const auto directory = path.parent_path();
        if (directory.empty()) return;
        change_ = FindFirstChangeNotificationW(
            directory.c_str(), FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME
                | FILE_NOTIFY_CHANGE_SIZE
                | FILE_NOTIFY_CHANGE_LAST_WRITE);
        if (change_ == INVALID_HANDLE_VALUE) change_ = nullptr;
    }

    [[nodiscard]] bool active() const noexcept { return change_ != nullptr; }

    void wait(
            HANDLE directive_event,
            std::chrono::milliseconds fallback_retry) {
        HANDLE handles[2]{};
        DWORD count{};
        if (directive_event) handles[count++] = directive_event;
        if (change_) handles[count++] = change_;
        const auto timeout = active()
            ? std::chrono::duration_cast<std::chrono::milliseconds>(
                kLocalGrowthFallbackRetry)
            : fallback_retry;
        const auto timeout_ms = static_cast<DWORD>(
            timeout.count());
        if (count == 0) {
            Sleep(timeout_ms);
            return;
        }
        const auto result = WaitForMultipleObjects(
            count, handles, FALSE, timeout_ms);
        const auto change_index = directive_event ? 1U : 0U;
        if (change_ && result == WAIT_OBJECT_0 + change_index) {
            if (!FindNextChangeNotification(change_)) reset();
        }
    }

private:
    void reset() noexcept {
        if (change_) FindCloseChangeNotification(change_);
        change_ = nullptr;
        source_.clear();
    }

    std::wstring source_;
    HANDLE change_{};
};

enum class DecoderReadStatus {
    frame,
    end_of_stream,
    retryable_failure,
    fatal_failure,
};

struct DecoderReadResult {
    DecoderReadStatus status{DecoderReadStatus::retryable_failure};
    std::shared_ptr<const DecodedD3D11VideoFrame> texture;
    HRESULT hresult{S_OK};
};

struct OutputTextureSlot {
    ID3D11Texture2D* texture{};
    std::uint32_t width{};
    std::uint32_t height{};
    MfD3D11VideoPixelFormat format{MfD3D11VideoPixelFormat::bgra8};
    std::weak_ptr<void> lease;

    OutputTextureSlot() = default;
    OutputTextureSlot(const OutputTextureSlot&) = delete;
    OutputTextureSlot& operator=(const OutputTextureSlot&) = delete;
    OutputTextureSlot(OutputTextureSlot&& other) noexcept
        : texture(std::exchange(other.texture, nullptr)),
          width(other.width),
          height(other.height),
          format(other.format),
          lease(std::move(other.lease)) {}
    OutputTextureSlot& operator=(OutputTextureSlot&& other) noexcept {
        if (this == &other) return *this;
        release(texture);
        texture = std::exchange(other.texture, nullptr);
        width = other.width;
        height = other.height;
        format = other.format;
        lease = std::move(other.lease);
        return *this;
    }
    ~OutputTextureSlot() { release(texture); }
};

struct OutputTextureAllocation {
    ID3D11Texture2D* texture{};
    std::shared_ptr<void> lease;
};

class DecoderSession {
public:
    DecoderSession(
            ID3D11Device* device,
            MfD3D11VideoDecoderConfig config,
            CastPipelineDiagnostics* diagnostics)
        : device_(device), config_(config), diagnostics_(diagnostics) {}
    ~DecoderSession() { close(); }

    [[nodiscard]] bool initialize() {
        last_hresult_ = device_ ? S_OK : E_POINTER;
        last_failure_fatal_ = !device_;
        return device_ != nullptr;
    }

    [[nodiscard]] bool open(const std::wstring& source) {
        close();
        last_hresult_ = S_OK;
        last_failure_fatal_ = false;
        if (source.empty() || !device_) {
            last_hresult_ = E_INVALIDARG;
            last_failure_fatal_ = true;
            return false;
        }
        if (open_ffmpeg(source, false)) return true;
        // A progressive local source may not have a complete container yet.
        // Keep the worker's existing growth notification and retry behavior.
        last_hresult_ = E_FAIL;
        return false;
    }
    void close() {
        release(ffmpeg_processor_);
        release(ffmpeg_enumerator_);
        ffmpeg_.close();
        using_ffmpeg_ = false;
        source_.clear();
        width_ = 0;
        height_ = 0;
        format_ = {};
        source_duration_seconds_ = 0.0;
        format_change_count_ = 0;
        output_textures_.clear();
    }

    [[nodiscard]] const std::wstring& source() const noexcept {
        return source_;
    }

    [[nodiscard]] HRESULT last_hresult() const noexcept {
        return last_hresult_;
    }

    [[nodiscard]] bool last_failure_is_fatal() const noexcept {
        return last_failure_fatal_;
    }

    [[nodiscard]] double source_duration_seconds() const noexcept {
        return source_duration_seconds_;
    }

    [[nodiscard]] std::uint64_t format_change_count() const noexcept {
        return format_change_count_;
    }

    [[nodiscard]] DecoderReadResult read_next_texture(
            std::uint64_t frame_id, std::uint64_t generation) {
        return read_ffmpeg_texture(frame_id, generation);
    }

    [[nodiscard]] bool seek(double seconds) {
        const auto succeeded = ffmpeg_.seek(seconds);
        last_hresult_ = succeeded ? S_OK : E_FAIL;
        last_failure_fatal_ = false;
        return succeeded;
    }
private:
    bool open_ffmpeg(const std::wstring& source, bool restricted) {
        if (!ffmpeg_.open(source, config_.output_format, restricted, device_)) return false;
        using_ffmpeg_ = true;
        source_ = source;
        source_duration_seconds_ = ffmpeg_.duration();
        last_hresult_ = S_OK;
        last_failure_fatal_ = false;
        log_line("Video decoder: using FFmpeg (D3D11VA preferred)");
        return true;
    }

    DecoderReadResult read_ffmpeg_texture(std::uint64_t frame_id, std::uint64_t generation) {
        FfmpegVideoFrame frame;
        const auto status = ffmpeg_.read(frame);
        if (status != FfmpegReadStatus::frame) {
            last_hresult_ = status == FfmpegReadStatus::end_of_stream ? S_OK : E_FAIL;
            last_failure_fatal_ = status == FfmpegReadStatus::fatal_error;
            return {status == FfmpegReadStatus::end_of_stream ? DecoderReadStatus::end_of_stream
                : last_failure_fatal_ ? DecoderReadStatus::fatal_failure
                : DecoderReadStatus::retryable_failure, {}, last_hresult_};
        }
        auto output = acquire_output_texture(frame.format.width, frame.format.height,
            frame.format.pixel_format);
        ID3D11DeviceContext* context{};
        if (output.texture) device_->GetImmediateContext(&context);
        if (!output.texture || !context) {
            release(context);
            release(output.texture);
            last_hresult_ = E_OUTOFMEMORY;
            last_failure_fatal_ = true;
            return {DecoderReadStatus::fatal_failure, {}, last_hresult_};
        }
        HRESULT upload = S_OK;
        if (frame.hardware_texture) {
            upload = copy_ffmpeg_hardware_frame(context, frame, output.texture);
        } else {
            context->UpdateSubresource(output.texture, 0, nullptr, frame.pixels.data(),
                frame.row_pitch, 0);
        }
        release(context);
        if (FAILED(upload)) {
            release(output.texture);
            last_hresult_ = upload;
            last_failure_fatal_ = true;
            return {DecoderReadStatus::fatal_failure, {}, upload};
        }
        format_ = frame.format;
        width_ = frame.format.width;
        height_ = frame.format.height;
        format_change_count_ = ffmpeg_.format_change_count();
        last_hresult_ = S_OK;
        last_failure_fatal_ = false;
        auto texture = std::make_shared<DecodedD3D11VideoFrame>(output.texture,
            std::move(output.lease), width_, height_, frame.timestamp_seconds,
            frame.duration_seconds, frame_id, generation, frame.format);
        return {DecoderReadStatus::frame, std::move(texture), S_OK};
    }

    HRESULT copy_ffmpeg_hardware_frame(ID3D11DeviceContext* context,
            const FfmpegVideoFrame& frame, ID3D11Texture2D* output) {
        auto* source = static_cast<ID3D11Texture2D*>(frame.hardware_texture);
        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        // NV12 remains on the GPU: copy the visible rectangle out of the
        // decoder's array texture into a leased, independently owned slot.
        if (config_.output_format == MfD3D11VideoPixelFormat::nv12
                && source_desc.Format == DXGI_FORMAT_NV12) {
            D3D11_BOX box{0, 0, 0, frame.format.width, frame.format.height, 1};
            context->CopySubresourceRegion(output, 0, 0, 0, 0, source,
                frame.hardware_subresource, &box);
            return S_OK;
        }
        ID3D11VideoDevice* video_device{};
        ID3D11VideoContext* video_context{};
        auto hr = device_->QueryInterface(IID_PPV_ARGS(&video_device));
        if (SUCCEEDED(hr)) hr = context->QueryInterface(IID_PPV_ARGS(&video_context));
        if (SUCCEEDED(hr) && (!ffmpeg_enumerator_ || !ffmpeg_processor_ || ffmpeg_processor_width_ != frame.format.width
                || ffmpeg_processor_height_ != frame.format.height)) {
            release(ffmpeg_processor_);
            release(ffmpeg_enumerator_);
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
            desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            desc.InputWidth = source_desc.Width;
            desc.InputHeight = source_desc.Height;
            desc.OutputWidth = frame.format.width;
            desc.OutputHeight = frame.format.height;
            desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            hr = video_device->CreateVideoProcessorEnumerator(&desc, &ffmpeg_enumerator_);
            if (SUCCEEDED(hr)) hr = video_device->CreateVideoProcessor(
                ffmpeg_enumerator_, 0, &ffmpeg_processor_);
            ffmpeg_processor_width_ = frame.format.width;
            ffmpeg_processor_height_ = frame.format.height;
        }
        ID3D11VideoProcessorInputView* input_view{};
        ID3D11VideoProcessorOutputView* output_view{};
        if (SUCCEEDED(hr)) {
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};
            desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            desc.Texture2D.ArraySlice = frame.hardware_subresource;
            hr = video_device->CreateVideoProcessorInputView(source, ffmpeg_enumerator_, &desc, &input_view);
        }
        if (SUCCEEDED(hr)) {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc{};
            desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            hr = video_device->CreateVideoProcessorOutputView(output, ffmpeg_enumerator_, &desc, &output_view);
        }
        if (SUCCEEDED(hr)) {
            RECT rectangle{0, 0, static_cast<LONG>(frame.format.width), static_cast<LONG>(frame.format.height)};
            video_context->VideoProcessorSetStreamSourceRect(ffmpeg_processor_, 0, TRUE, &rectangle);
            video_context->VideoProcessorSetStreamDestRect(ffmpeg_processor_, 0, TRUE, &rectangle);
            video_context->VideoProcessorSetOutputTargetRect(ffmpeg_processor_, TRUE, &rectangle);
            video_context->VideoProcessorSetStreamAutoProcessingMode(ffmpeg_processor_, 0, FALSE);
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_colour{};
            input_colour.YCbCr_Matrix = frame.format.yuv_matrix == MFVideoTransferMatrix_BT601 ? 0 : 1;
            input_colour.Nominal_Range = frame.hardware_full_range ? 2 : 1;
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_colour = input_colour;
            if (config_.output_format == MfD3D11VideoPixelFormat::bgra8) {
                output_colour.RGB_Range = 0;
                output_colour.Nominal_Range = 2;
            }
            video_context->VideoProcessorSetStreamColorSpace(ffmpeg_processor_, 0, &input_colour);
            video_context->VideoProcessorSetOutputColorSpace(ffmpeg_processor_, &output_colour);
            D3D11_VIDEO_PROCESSOR_STREAM stream{};
            stream.Enable = TRUE;
            stream.pInputSurface = input_view;
            hr = video_context->VideoProcessorBlt(ffmpeg_processor_, output_view, 0, 1, &stream);
        }
        release(input_view);
        release(output_view);
        release(video_context);
        release(video_device);
        return hr;
    }

    [[nodiscard]] OutputTextureAllocation acquire_output_texture(
            std::uint32_t width,
            std::uint32_t height,
            MfD3D11VideoPixelFormat format) {
        for (auto& slot : output_textures_) {
            if (slot.width != width || slot.height != height
                    || slot.format != format
                    || !slot.lease.expired()) {
                continue;
            }
            auto lease = std::make_shared<unsigned char>(0);
            slot.lease = lease;
            slot.texture->AddRef();
            return OutputTextureAllocation{
                slot.texture, std::move(lease)};
        }

        D3D11_TEXTURE2D_DESC output_desc{};
        output_desc.Width = width;
        output_desc.Height = height;
        output_desc.MipLevels = 1;
        output_desc.ArraySize = 1;
        output_desc.Format = format == MfD3D11VideoPixelFormat::nv12
            ? DXGI_FORMAT_NV12 : DXGI_FORMAT_B8G8R8A8_UNORM;
        output_desc.SampleDesc.Count = 1;
        output_desc.Usage = D3D11_USAGE_DEFAULT;
        output_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (using_ffmpeg_) output_desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
        if (format == MfD3D11VideoPixelFormat::nv12) {
            // The copied frame is the stable decoder-owned source consumed by
            // Cast's video processor. Some drivers reject an input view unless
            // the resource declares decoder/video-input compatibility.
            if (!using_ffmpeg_) output_desc.BindFlags |= D3D11_BIND_DECODER;
        }
        ID3D11Texture2D* texture{};
        if (FAILED(device_->CreateTexture2D(
                &output_desc, nullptr, &texture)) || !texture) {
            release(texture);
            return {};
        }
        if (diagnostics_) {
            diagnostics_->increment(CastPipelineCounter::texture_creations);
        }

        if (output_textures_.size()
                >= config_.max_queued_frames + 2U) {
            return OutputTextureAllocation{texture, {}};
        }

        auto lease = std::make_shared<unsigned char>(0);
        OutputTextureSlot slot;
        slot.texture = texture;
        slot.width = width;
        slot.height = height;
        slot.format = format;
        slot.lease = lease;
        output_textures_.push_back(std::move(slot));
        texture->AddRef();
        return OutputTextureAllocation{texture, std::move(lease)};
    }

    ID3D11Device* device_{};
    MfD3D11VideoDecoderConfig config_;
    FfmpegVideoDecoder ffmpeg_;
    bool using_ffmpeg_{};
    ID3D11VideoProcessorEnumerator* ffmpeg_enumerator_{};
    ID3D11VideoProcessor* ffmpeg_processor_{};
    std::uint32_t ffmpeg_processor_width_{};
    std::uint32_t ffmpeg_processor_height_{};
    std::wstring source_;
    std::uint32_t width_{};
    std::uint32_t height_{};
    MfD3D11VideoFormat format_;
    HRESULT last_hresult_{S_OK};
    bool last_failure_fatal_{};
    double source_duration_seconds_{};
    std::uint64_t format_change_count_{};
    std::vector<OutputTextureSlot> output_textures_;
    CastPipelineDiagnostics* diagnostics_{};
};

}  // namespace

DecodedD3D11VideoFrame::DecodedD3D11VideoFrame(
        void* texture,
        std::shared_ptr<void> texture_lease,
        std::uint32_t width,
        std::uint32_t height,
        double timestamp_seconds,
        double duration_seconds,
        std::uint64_t frame_id,
        std::uint64_t generation,
        MfD3D11VideoFormat format) noexcept
    : texture_(texture),
      texture_lease_(std::move(texture_lease)),
      width_(width),
      height_(height),
      timestamp_seconds_(timestamp_seconds),
      duration_seconds_(duration_seconds),
      frame_id_(frame_id),
      generation_(generation),
      format_(format) {
    // Preserve constructor compatibility for synthetic frames while ensuring
    // real decoder frames always expose dimensions through one source of truth.
    if (!format_.valid()) {
        format_.width = width;
        format_.height = height;
    }
}

DecodedD3D11VideoFrame::~DecodedD3D11VideoFrame() {
    auto* texture = static_cast<ID3D11Texture2D*>(texture_);
    release(texture);
    texture_ = nullptr;
}

void* DecodedD3D11VideoFrame::texture() const noexcept { return texture_; }
std::uint32_t DecodedD3D11VideoFrame::width() const noexcept { return width_; }
std::uint32_t DecodedD3D11VideoFrame::height() const noexcept { return height_; }
double DecodedD3D11VideoFrame::timestamp_seconds() const noexcept {
    return timestamp_seconds_;
}
double DecodedD3D11VideoFrame::duration_seconds() const noexcept {
    return duration_seconds_;
}
std::uint64_t DecodedD3D11VideoFrame::frame_id() const noexcept {
    return frame_id_;
}
std::uint64_t DecodedD3D11VideoFrame::generation() const noexcept {
    return generation_;
}
const MfD3D11VideoFormat& DecodedD3D11VideoFrame::format() const noexcept {
    return format_;
}
std::shared_ptr<void>
DecodedD3D11VideoFrame::retention_token() const noexcept {
    return texture_lease_;
}

struct MfD3D11VideoDecoder::Impl {
    mutable std::mutex mutex;
    std::condition_variable request_ready;
    std::condition_variable startup_ready;
    std::thread worker;
    ID3D11Device* device{};
    bool stop_requested{};
    bool startup_complete{};
    bool startup_succeeded{};
    PlaybackDirective directive;
    DecodedTextureQueue decoded;
    MfD3D11VideoDecoderConfig config;
    CastPipelineDiagnostics* diagnostics{};
    HANDLE retry_wake_event{};

    bool start(
            void* borrowed_device,
            const MfD3D11VideoDecoderConfig& requested_config,
            CastPipelineDiagnostics* requested_diagnostics) {
        auto* requested_device = static_cast<ID3D11Device*>(borrowed_device);
        if (!requested_device) return false;
        const auto normalized_config = sanitize_decoder_config(
            requested_config);
        std::unique_lock lock(mutex);
        if (worker.joinable()) return device == requested_device
            && config.output_format == normalized_config.output_format
            && config.max_queued_frames
                == normalized_config.max_queued_frames
            && config.max_queued_bytes
                == normalized_config.max_queued_bytes
            && config.decode_ahead_seconds
                == normalized_config.decode_ahead_seconds
            && config.observe_local_source_growth
                == normalized_config.observe_local_source_growth
            && diagnostics == requested_diagnostics;
        retry_wake_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!retry_wake_event) return false;
        requested_device->AddRef();
        device = requested_device;
        config = normalized_config;
        diagnostics = requested_diagnostics;
        stop_requested = false;
        startup_complete = false;
        startup_succeeded = false;
        try {
            worker = std::thread([this] { worker_main(); });
        } catch (...) {
            release(device);
            CloseHandle(retry_wake_event);
            retry_wake_event = nullptr;
            diagnostics = nullptr;
            stop_requested = true;
            return false;
        }
        startup_ready.wait(lock, [&] { return startup_complete; });
        if (startup_succeeded) return true;

        stop_requested = true;
        ++directive.serial;
        SetEvent(retry_wake_event);
        lock.unlock();
        request_ready.notify_one();
        worker.join();
        lock.lock();
        release(device);
        CloseHandle(retry_wake_event);
        retry_wake_event = nullptr;
        diagnostics = nullptr;
        directive = {};
        decoded = {};
        stop_requested = false;
        return false;
    }

    void suspend() {
        {
            std::lock_guard lock(mutex);
            const auto next_serial = directive.serial + 1;
            const auto next_seek_epoch = directive.seek_epoch + 1;
            directive = {};
            directive.serial = next_serial;
            directive.seek_epoch = next_seek_epoch;
            decoded = {};
            if (retry_wake_event) SetEvent(retry_wake_event);
        }
        request_ready.notify_one();
    }

    void stop() {
        {
            std::lock_guard lock(mutex);
            if (!worker.joinable()) {
                release(device);
                if (retry_wake_event) CloseHandle(retry_wake_event);
                retry_wake_event = nullptr;
                diagnostics = nullptr;
                directive = {};
                decoded = {};
                return;
            }
            stop_requested = true;
            ++directive.serial;
            if (retry_wake_event) SetEvent(retry_wake_event);
        }
        request_ready.notify_one();
        worker.join();
        std::lock_guard lock(mutex);
        release(device);
        if (retry_wake_event) CloseHandle(retry_wake_event);
        retry_wake_event = nullptr;
        diagnostics = nullptr;
        directive = {};
        decoded = {};
        stop_requested = false;
        startup_complete = false;
        startup_succeeded = false;
    }

    static bool matches(
            const DecodedTextureQueue& queue,
            const PlaybackDirective& target) {
        return queue.source == target.source
            && queue.timeline_revision == target.timeline_revision
            && queue.seek_epoch == target.seek_epoch;
    }

    static void prune_before(
            DecodedTextureQueue& queue, double position_seconds) {
        const auto limit = sanitize_position(position_seconds)
            + kTimestampToleranceSeconds;
        while (queue.textures.size() > 1
                && queue.textures[1]->timestamp_seconds() <= limit) {
            queue.textures.pop_front();
        }
    }

    bool needs_decode(
            const DecodedTextureQueue& queue,
            const PlaybackDirective& target) const {
        if (target.state == MfD3D11PlaybackState::idle
                || target.source.empty()) {
            return false;
        }
        if (!matches(queue, target)) return true;
        if (queue.availability
                == MfD3D11VideoAvailability::fatal_error) {
            return false;
        }
        if (queue.end_of_stream) return false;
        if (queue.textures.empty()) return true;
        if (queue.textures.size()
                >= effective_queue_frame_limit(queue, config)) {
            return false;
        }
        const auto ahead = target.state == MfD3D11PlaybackState::playing
            ? config.decode_ahead_seconds : 0.0;
        return contiguous_buffer_end_seconds(queue)
            + kTimestampToleranceSeconds
            < target.position_seconds + ahead;
    }

    [[nodiscard]] bool retry_cancelled(
            const PlaybackDirective& work) const {
        return stop_requested
            || directive.source != work.source
            || directive.timeline_revision != work.timeline_revision
            || directive.seek_epoch != work.seek_epoch
            || directive.state != work.state
            || directive.state == MfD3D11PlaybackState::idle;
    }

    void wait_for_source_retry(
            const PlaybackDirective& work,
            LocalSourceGrowthNotification& source_growth) {
        if (retry_wake_event) ResetEvent(retry_wake_event);
        {
            std::lock_guard lock(mutex);
            if (retry_cancelled(work)) return;
        }
        source_growth.wait(retry_wake_event, kDefaultSourceRetry);
    }

    void synchronize(
            const std::wstring& source,
            std::uint64_t timeline_revision,
            double position_seconds,
            MfD3D11PlaybackState state) {
        const auto position = sanitize_position(position_seconds);
        bool interrupt_retry{};
        {
            std::lock_guard lock(mutex);
            if (!worker.joinable() || stop_requested) return;
            if (directive.source == source
                    && directive.timeline_revision == timeline_revision
                    && directive.state == state
                    && std::abs(directive.position_seconds - position)
                        < 0.002) {
                return;
            }
            const bool identity_changed = directive.source != source
                || directive.timeline_revision != timeline_revision;
            const bool state_changed = directive.state != state;
            const bool position_discontinuity = !identity_changed
                && !source.empty()
                && state != MfD3D11PlaybackState::idle
                && std::abs(directive.position_seconds - position)
                    >= kPlaybackPositionDiscontinuitySeconds;
            if (identity_changed || position_discontinuity) {
                ++directive.seek_epoch;
            }
            interrupt_retry = identity_changed || position_discontinuity
                || state_changed;
            directive.source = source;
            directive.timeline_revision = timeline_revision;
            directive.position_seconds = position;
            directive.state = state;
            ++directive.serial;
            if (state == MfD3D11PlaybackState::idle || source.empty()
                    || identity_changed || position_discontinuity) {
                decoded = {};
                if (state != MfD3D11PlaybackState::idle
                        && !source.empty()) {
                    decoded.source = source;
                    decoded.timeline_revision = timeline_revision;
                    decoded.seek_epoch = directive.seek_epoch;
                    decoded.availability =
                        MfD3D11VideoAvailability::opening;
                }
            } else if (matches(decoded, directive)) {
                prune_before(decoded, position);
            }
            if (interrupt_retry && retry_wake_event) {
                SetEvent(retry_wake_event);
            }
        }
        request_ready.notify_one();
    }

    std::shared_ptr<const DecodedD3D11VideoFrame> texture_for(
            const std::wstring& source,
            std::uint64_t timeline_revision,
            double position_seconds) const {
        std::lock_guard lock(mutex);
        if (decoded.source != source
                || decoded.timeline_revision != timeline_revision
                || decoded.textures.empty()) {
            return {};
        }
        if (decoded.end_of_stream
                && directive.state == MfD3D11PlaybackState::playing) {
            const auto& last = decoded.textures.back();
            const auto frame_end = last->timestamp_seconds()
                + std::max(last->duration_seconds(),
                    kTimestampToleranceSeconds);
            if (sanitize_position(position_seconds)
                    > frame_end + kTimestampToleranceSeconds) {
                return {};
            }
        }
        const auto limit = sanitize_position(position_seconds)
            + kTimestampToleranceSeconds;
        auto selected = decoded.textures.front();
        for (const auto& texture : decoded.textures) {
            if (texture->timestamp_seconds() > limit) break;
            selected = texture;
        }
        return selected;
    }

    MfD3D11VideoBufferStatus buffer_status(
            const std::wstring& source,
            std::uint64_t timeline_revision) const {
        std::lock_guard lock(mutex);
        MfD3D11VideoBufferStatus status;
        if (decoded.source != source
                || decoded.timeline_revision != timeline_revision) {
            return status;
        }
        status.frame_count = decoded.textures.size();
        status.end_of_stream = decoded.end_of_stream;
        status.availability = decoded.availability;
        status.hresult = static_cast<std::int32_t>(decoded.hresult);
        status.source_duration_seconds = decoded.source_duration_seconds;
        status.format_change_count = decoded.format_change_count;
        if (decoded.textures.empty()) return status;
        const auto& first = decoded.textures.front();
        status.buffered_start_seconds = first->timestamp_seconds();
        status.buffered_end_seconds =
            contiguous_buffer_end_seconds(decoded);
        status.buffered_duration_seconds = std::max(0.0,
            status.buffered_end_seconds - status.buffered_start_seconds);
        return status;
    }

    void worker_main() {
        const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DecoderSession session(device, config, diagnostics);
        const bool initialized = session.initialize();
        {
            std::lock_guard lock(mutex);
            startup_succeeded = initialized;
            startup_complete = true;
        }
        startup_ready.notify_one();
        if (!initialized) {
            if (SUCCEEDED(com)) CoUninitialize();
            return;
        }

        std::uint64_t handled_serial{};
        std::wstring session_source;
        std::uint64_t session_revision{};
        std::uint64_t session_seek_epoch{};
        std::uint64_t next_frame_id{};
        bool decoder_underflow{};
        std::uint64_t frame_identity_revision{};
        std::uint64_t frame_identity_seek_epoch{};
        LocalSourceGrowthNotification source_growth;

        for (;;) {
            PlaybackDirective work;
            bool reopen_session = false;
            bool seek_session = false;
            bool decode_required = false;
            {
                std::unique_lock lock(mutex);
                request_ready.wait(lock, [&] {
                    return stop_requested
                        || directive.serial != handled_serial
                        || needs_decode(decoded, directive);
                });
                if (stop_requested) break;
                work = directive;
                handled_serial = directive.serial;
                prune_before(decoded, work.position_seconds);
                reopen_session = session_source != work.source
                    || session_revision != work.timeline_revision;
                seek_session = reopen_session
                    || session_seek_epoch != work.seek_epoch;
                decode_required = seek_session || needs_decode(decoded, work);
            }

            if (work.state == MfD3D11PlaybackState::idle
                    || work.source.empty()) {
                source_growth.observe(std::wstring{});
                session.close();
                session_source.clear();
                session_revision = 0;
                session_seek_epoch = 0;
                continue;
            }
            source_growth.observe(config.observe_local_source_growth
                ? work.source : std::wstring{});
            if (!decode_required) continue;

            if (seek_session) {
                bool session_ready = true;
                if (reopen_session) {
                    session.close();
                    session_source.clear();
                    session_revision = 0;
                    session_seek_epoch = 0;
                    session_ready = session.open(work.source);
                }
                if (!session_ready || !session.seek(work.position_seconds)) {
                    const auto failure = session.last_hresult();
                    const bool fatal = session.last_failure_is_fatal();
                    {
                        std::lock_guard lock(mutex);
                        if (directive.source == work.source
                                && directive.timeline_revision
                                    == work.timeline_revision
                                && directive.seek_epoch == work.seek_epoch) {
                            if (!matches(decoded, work)) {
                                decoded.source = work.source;
                                decoded.timeline_revision =
                                    work.timeline_revision;
                                decoded.seek_epoch = work.seek_epoch;
                                decoded.textures.clear();
                            }
                            decoded.end_of_stream = false;
                            decoded.availability = fatal
                                ? MfD3D11VideoAvailability::fatal_error
                                : MfD3D11VideoAvailability::
                                    retryable_starvation;
                            decoded.hresult = failure;
                        }
                    }
                    session.close();
                    session_source.clear();
                    session_revision = 0;
                    session_seek_epoch = 0;
                    if (fatal) continue;
                    wait_for_source_retry(work, source_growth);
                    continue;
                }
                session_source = work.source;
                session_revision = work.timeline_revision;
                session_seek_epoch = work.seek_epoch;
                if (frame_identity_revision != work.timeline_revision
                        || frame_identity_seek_epoch != work.seek_epoch) {
                    next_frame_id = 0;
                    decoder_underflow = false;
                    frame_identity_revision = work.timeline_revision;
                    frame_identity_seek_epoch = work.seek_epoch;
                }
                std::lock_guard lock(mutex);
                if (directive.source == work.source
                        && directive.timeline_revision
                            == work.timeline_revision
                        && directive.seek_epoch == work.seek_epoch) {
                    if (!matches(decoded, work)) {
                        decoded.source = work.source;
                        decoded.timeline_revision = work.timeline_revision;
                        decoded.seek_epoch = work.seek_epoch;
                        decoded.textures.clear();
                    }
                    decoded.end_of_stream = false;
                    decoded.availability =
                        MfD3D11VideoAvailability::opening;
                    decoded.hresult = S_OK;
                    decoded.source_duration_seconds =
                        session.source_duration_seconds();
                    decoded.format_change_count =
                        session.format_change_count();
                }
            }

            // Burst-decode until both the configured time window and bounded
            // frame/byte capacity are satisfied. Native Stage retains its
            // historical shallow defaults; Cast opts into a deeper runway.
            while (true) {
                {
                    std::lock_guard lock(mutex);
                    if (directive.source != work.source
                            || directive.timeline_revision
                                != work.timeline_revision
                            || directive.seek_epoch != work.seek_epoch
                            || directive.state
                                == MfD3D11PlaybackState::idle) {
                        break;
                    }
                    prune_before(decoded,
                        directive.position_seconds);
                    if (!needs_decode(decoded, directive)) break;
                }

                const auto correlation_id = next_frame_id;
                if (diagnostics) {
                    diagnostics->record(
                        CastPipelineEvent::decoder_request,
                        correlation_id,
                        work.timeline_revision);
                }
                auto result = session.read_next_texture(
                    correlation_id, work.timeline_revision);
                if (result.status == DecoderReadStatus::end_of_stream) {
                    std::lock_guard lock(mutex);
                    if (matches(decoded, work)) {
                        decoded.end_of_stream = true;
                        decoded.availability =
                            MfD3D11VideoAvailability::end_of_stream;
                        decoded.hresult = S_OK;
                        decoded.source_duration_seconds =
                            session.source_duration_seconds();
                        decoded.format_change_count =
                            session.format_change_count();
                    }
                    break;
                }
                if (result.status == DecoderReadStatus::fatal_failure) {
                    std::lock_guard lock(mutex);
                    if (matches(decoded, work)) {
                        decoded.end_of_stream = false;
                        decoded.availability =
                            MfD3D11VideoAvailability::fatal_error;
                        decoded.hresult = result.hresult;
                        decoded.source_duration_seconds =
                            session.source_duration_seconds();
                        decoded.format_change_count =
                            session.format_change_count();
                    }
                    break;
                }
                if (result.status
                        == DecoderReadStatus::retryable_failure) {
                    // Progressive HTTP sources can temporarily run
                    // out of readable data or invalidate a
                    // SourceReader while the file grows.  That is
                    // not media EOS: reopen at the latest playback
                    // position.
                    if (!decoder_underflow && diagnostics) {
                        diagnostics->increment(
                            CastPipelineCounter::decoder_underflows);
                        diagnostics->record(
                            CastPipelineEvent::underflow,
                            correlation_id,
                            work.timeline_revision);
                    }
                    decoder_underflow = true;
                    {
                        std::lock_guard lock(mutex);
                        if (matches(decoded, work)) {
                            decoded.end_of_stream = false;
                            decoded.availability =
                                MfD3D11VideoAvailability::
                                    retryable_starvation;
                            decoded.hresult = result.hresult;
                            decoded.source_duration_seconds =
                                session.source_duration_seconds();
                            decoded.format_change_count =
                                session.format_change_count();
                        }
                    }
                    session.close();
                    session_source.clear();
                    session_revision = 0;
                    session_seek_epoch = 0;
                    wait_for_source_retry(work, source_growth);
                    break;
                }
                auto texture = std::move(result.texture);
                if (!texture) continue;
                if (diagnostics) {
                    diagnostics->increment(
                        CastPipelineCounter::decoded_frames);
                    diagnostics->record(
                        CastPipelineEvent::decoder_sample_ready,
                        correlation_id,
                        work.timeline_revision);
                    if (decoder_underflow) {
                        diagnostics->increment(
                            CastPipelineCounter::decoder_recoveries);
                        diagnostics->record(
                            CastPipelineEvent::recovered,
                            correlation_id,
                            work.timeline_revision);
                    }
                }
                decoder_underflow = false;
                ++next_frame_id;

                std::lock_guard lock(mutex);
                if (directive.source != work.source
                        || directive.timeline_revision
                            != work.timeline_revision
                        || directive.seek_epoch != work.seek_epoch
                        || directive.state
                            == MfD3D11PlaybackState::idle) {
                    break;
                }
                if (!matches(decoded, work)) {
                    decoded.source = work.source;
                    decoded.timeline_revision
                        = work.timeline_revision;
                    decoded.seek_epoch = work.seek_epoch;
                    decoded.textures.clear();
                    decoded.end_of_stream = false;
                }
                if (decoded.textures.size()
                        < effective_queue_frame_limit(decoded, config)
                        && (decoded.textures.empty()
                        || texture->timestamp_seconds()
                            > decoded.textures.back()
                                ->timestamp_seconds()
                                + kTimestampToleranceSeconds)) {
                    decoded.textures.push_back(
                        std::move(texture));
                }
                decoded.availability =
                    MfD3D11VideoAvailability::ready;
                decoded.hresult = S_OK;
                decoded.source_duration_seconds =
                    session.source_duration_seconds();
                decoded.format_change_count =
                    session.format_change_count();
            }
        }

        session.close();
        if (SUCCEEDED(com)) CoUninitialize();
    }
};

MfD3D11VideoDecoder::MfD3D11VideoDecoder()
    : impl_(std::make_unique<Impl>()) {}

MfD3D11VideoDecoder::~MfD3D11VideoDecoder() { stop(); }

void MfD3D11VideoDecoder::swap(
        MfD3D11VideoDecoder& other) noexcept {
    impl_.swap(other.impl_);
}

bool MfD3D11VideoDecoder::start(
        void* d3d11_device,
        CastPipelineDiagnostics* diagnostics) {
    return start(d3d11_device, MfD3D11VideoDecoderConfig{}, diagnostics);
}

bool MfD3D11VideoDecoder::start(
        void* d3d11_device,
        const MfD3D11VideoDecoderConfig& config,
        CastPipelineDiagnostics* diagnostics) {
    return impl_ && impl_->start(d3d11_device, config, diagnostics);
}

void MfD3D11VideoDecoder::suspend() {
    if (impl_) impl_->suspend();
}

void MfD3D11VideoDecoder::stop() {
    if (impl_) impl_->stop();
}

void MfD3D11VideoDecoder::synchronize(
        const std::wstring& source,
        std::uint64_t timeline_revision,
        double position_seconds,
        MfD3D11PlaybackState state) {
    if (impl_) {
        impl_->synchronize(
            source, timeline_revision, position_seconds, state);
    }
}

std::shared_ptr<const DecodedD3D11VideoFrame>
MfD3D11VideoDecoder::texture_for(
        const std::wstring& source,
        std::uint64_t timeline_revision,
        double position_seconds) const {
    return impl_ ? impl_->texture_for(
        source, timeline_revision, position_seconds) : nullptr;
}

MfD3D11VideoBufferStatus MfD3D11VideoDecoder::buffer_status(
        const std::wstring& source,
        std::uint64_t timeline_revision) const {
    return impl_ ? impl_->buffer_status(source, timeline_revision)
                 : MfD3D11VideoBufferStatus{};
}
