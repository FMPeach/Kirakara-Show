#include "mf_d3d11_video_decoder.h"

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
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <propvarutil.h>

// MF_XVP_DISABLE_FRC is declared by the Windows SDK's mfidl.h, but the MinGW
// headers shipped with this toolchain only carry MF_XVP_SAMPLE_LOCK_TIMEOUT.
// Value taken from Wine's mfidl.idl. TRUE disables frame-rate conversion in
// the video processor MFT (deinterlacing still runs).
#ifndef MF_XVP_DISABLE_FRC
static const GUID MF_XVP_DISABLE_FRC = {
    0x2c0afa19, 0x7a97, 0x4d5a,
    {0x9e, 0xe8, 0x16, 0xd4, 0xfc, 0x51, 0x8d, 0x8c}};
#endif

namespace {

constexpr double kTimestampToleranceSeconds = 1.0 / 240.0;
constexpr double kDefaultFrameDurationSeconds = 1.0 / 30.0;
constexpr double kMaximumContinuityGapSeconds = 0.100;
// MinGW exposes MF_MT_VIDEO_ROTATION only when WINVER >= Windows 8, while
// querying an unknown media-type attribute is harmless on older runtimes.
// Keep the decoder's existing Windows target and use the published key value.
constexpr GUID kMfMtVideoRotation{
    0xc380465d, 0x2271, 0x428c,
    {0x9b, 0x83, 0xec, 0xea, 0x3b, 0x4a, 0x85, 0xc1}};
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

std::wstring source_url(const std::wstring& source) {
    if (source.starts_with(L"http://") || source.starts_with(L"https://")) {
        return source;
    }
    return file_url_from_path(source);
}

double sanitize_position(double seconds) {
    return std::isfinite(seconds) && seconds > 0.0 ? seconds : 0.0;
}

LONGLONG hns_from_seconds(double seconds) {
    return static_cast<LONGLONG>(sanitize_position(seconds) * 10000000.0);
}

double seconds_from_hns(LONGLONG value) {
    return static_cast<double>(value) / 10000000.0;
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

bool is_fatal_decoder_hresult(HRESULT hr) {
    return hr == MF_E_INVALIDMEDIATYPE
        || hr == MF_E_UNSUPPORTED_REPRESENTATION
        || hr == MF_E_INVALID_FILE_FORMAT
        || hr == MF_E_UNSUPPORTED_BYTESTREAM_TYPE
        || hr == MF_E_TOPO_CODEC_NOT_FOUND
        || hr == E_NOINTERFACE
        || hr == E_UNEXPECTED
        || hr == DXGI_ERROR_DEVICE_REMOVED
        || hr == DXGI_ERROR_DEVICE_RESET;
}

DecoderReadStatus failure_status(HRESULT hr) {
    return is_fatal_decoder_hresult(hr)
        ? DecoderReadStatus::fatal_failure
        : DecoderReadStatus::retryable_failure;
}

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
    ~DecoderSession() { close(); release(device_manager_); }

    [[nodiscard]] bool initialize() {
        if (!device_) {
            last_hresult_ = E_POINTER;
            last_failure_fatal_ = true;
            return false;
        }
        auto hr = MFCreateDXGIDeviceManager(
            &device_manager_reset_token_, &device_manager_);
        if (SUCCEEDED(hr)) {
            hr = device_manager_->ResetDevice(
                device_, device_manager_reset_token_);
        }
        if (FAILED(hr)) {
            last_hresult_ = hr;
            last_failure_fatal_ = true;
            log_hresult("MF D3D decoder: device manager failed", hr);
            release(device_manager_);
            return false;
        }
        last_hresult_ = S_OK;
        last_failure_fatal_ = false;
        return true;
    }

    [[nodiscard]] bool open(const std::wstring& source) {
        close();
        last_hresult_ = S_OK;
        last_failure_fatal_ = false;
        if (source.empty() || !device_manager_) {
            last_hresult_ = E_INVALIDARG;
            last_failure_fatal_ = true;
            return false;
        }

        IMFAttributes* attributes{};
        auto hr = MFCreateAttributes(&attributes, 6);
        if (SUCCEEDED(hr)) {
            hr = attributes->SetUnknown(
                MF_SOURCE_READER_D3D_MANAGER, device_manager_);
        }
        if (SUCCEEDED(hr)) {
            hr = attributes->SetUINT32(
                MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        }
        if (SUCCEEDED(hr)) {
            hr = attributes->SetUINT32(
                MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        }
        const auto url = source_url(source);
        if (SUCCEEDED(hr)) {
            hr = MFCreateSourceReaderFromURL(
                url.c_str(), attributes, &reader_);
        }
        release(attributes);
        if (FAILED(hr) || !reader_) {
            last_hresult_ = FAILED(hr) ? hr : E_UNEXPECTED;
            // The app can point the reader at a progressively growing local
            // file. Until its container header is present even errors that
            // normally describe an unsupported stream can be temporary.
            last_failure_fatal_ = false;
            log_hresult("MF D3D decoder: create source failed", hr);
            close();
            return false;
        }

        PROPVARIANT source_duration{};
        PropVariantInit(&source_duration);
        if (SUCCEEDED(reader_->GetPresentationAttribute(
                MF_SOURCE_READER_MEDIASOURCE,
                MF_PD_DURATION,
                &source_duration))) {
            LONGLONG duration_hns{};
            if (source_duration.vt == VT_UI8) {
                duration_hns = static_cast<LONGLONG>(
                    source_duration.uhVal.QuadPart);
            } else if (source_duration.vt == VT_I8) {
                duration_hns = source_duration.hVal.QuadPart;
            }
            if (duration_hns > 0) {
                source_duration_seconds_ = seconds_from_hns(duration_hns);
            }
        }
        PropVariantClear(&source_duration);

        static_cast<void>(
            reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE));
        static_cast<void>(reader_->SetStreamSelection(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE));

        if (!apply_output_media_type() || !update_format()) {
            close();
            return false;
        }

        source_ = source;
        last_hresult_ = S_OK;
        last_failure_fatal_ = false;
        return true;
    }

    void close() {
        release(reader_);
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
            std::uint64_t frame_id,
            std::uint64_t generation) {
        if (!reader_) {
            last_hresult_ = E_UNEXPECTED;
            last_failure_fatal_ = false;
            return DecoderReadResult{
                DecoderReadStatus::retryable_failure, {}, last_hresult_};
        }
        IMFSample* sample{};
        LONGLONG timestamp{};
        LONGLONG duration{};
        const auto status = read_next_sample(sample, timestamp, duration);
        if (status != DecoderReadStatus::frame) {
            return DecoderReadResult{status, {}, last_hresult_};
        }
        auto texture = texture_from_sample(
            sample, timestamp, duration, frame_id, generation);
        release(sample);
        if (!texture) {
            return DecoderReadResult{
                last_failure_fatal_
                    ? DecoderReadStatus::fatal_failure
                    : DecoderReadStatus::retryable_failure,
                {}, last_hresult_};
        }
        return DecoderReadResult{
            DecoderReadStatus::frame, std::move(texture), S_OK};
    }

    [[nodiscard]] bool seek(double seconds) {
        return seek_internal(seconds);
    }

private:
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
        if (format == MfD3D11VideoPixelFormat::nv12) {
            // The copied frame is the stable decoder-owned source consumed by
            // Cast's video processor. Some drivers reject an input view unless
            // the resource declares decoder/video-input compatibility.
            output_desc.BindFlags |= D3D11_BIND_DECODER;
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

    // The reader is created with MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_
    // PROCESSING so it can hand back ARGB32, which means it inserts a video
    // processor MFT after the decoder. That processor does more than colour
    // conversion: it also performs frame-rate conversion and inverse telecine,
    // and it implements both by REPEATING pictures. On a stream that declares
    // MFVideoInterlace_MixedInterlaceOrProgressive it runs in adaptive mode
    // and, after a mid-stream parameter-set change, starts emitting the same
    // picture under fresh timestamps -- every timing counter stays clean while
    // the image stops advancing.
    //
    // Reaching the MFTs directly and switching frame-rate conversion off is
    // what actually stops it; pinning the output media type alone does not.
    void configure_chain_transforms() {
        if (!reader_) return;
        IMFSourceReaderEx* reader_ex{};
        if (FAILED(reader_->QueryInterface(IID_PPV_ARGS(&reader_ex)))
                || !reader_ex) {
            log_line("MF D3D decoder: IMFSourceReaderEx unavailable;"
                " video processor may repeat frames");
            return;
        }
        for (DWORD index = 0; index < 8; ++index) {
            GUID category{};
            IMFTransform* transform{};
            if (FAILED(reader_ex->GetTransformForStream(
                        MF_SOURCE_READER_FIRST_VIDEO_STREAM, index,
                        &category, &transform))
                    || !transform) {
                release(transform);
                break;
            }
            IMFAttributes* attributes{};
            if (SUCCEEDED(transform->GetAttributes(&attributes))
                    && attributes) {
                static_cast<void>(
                    attributes->SetUINT32(MF_XVP_DISABLE_FRC, TRUE));
            }
            release(attributes);
            release(transform);
        }
        release(reader_ex);
    }

    // BGRA output mirrors source timing and geometry so an unavoidable colour
    // conversion processor has no additional conversion to justify. Native
    // NV12 deliberately requests only the subtype: pinning compressed-source
    // metadata onto the uncompressed output makes Source Reader insert XVP on
    // some drivers. Advanced processing still has to be enabled at reader
    // creation time so the hardware decoder emits every inter-frame sample;
    // a matching bare NV12 type keeps the resulting chain decoder-only.
    [[nodiscard]] bool apply_output_media_type() {
        if (!reader_) return false;

        const auto requested_subtype = config_.output_format
                == MfD3D11VideoPixelFormat::nv12
            ? MFVideoFormat_NV12 : MFVideoFormat_ARGB32;

        IMFMediaType* native{};
        static_cast<void>(reader_->GetNativeMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native));

        const auto build = [&](bool pin) -> IMFMediaType* {
            IMFMediaType* type{};
            if (FAILED(MFCreateMediaType(&type))) return nullptr;
            const bool ok = SUCCEEDED(
                    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))
                && SUCCEEDED(
                    type->SetGUID(MF_MT_SUBTYPE, requested_subtype));
            if (!ok) {
                release(type);
                return nullptr;
            }
            if (config_.output_format == MfD3D11VideoPixelFormat::nv12) {
                return type;
            }
            if (!pin || !native) return type;

            const auto copy_uint32 = [&](REFGUID key) {
                UINT32 value{};
                if (SUCCEEDED(native->GetUINT32(key, &value))) {
                    static_cast<void>(type->SetUINT32(key, value));
                }
            };
            const auto copy_blob = [&](REFGUID key) {
                UINT32 size{};
                if (FAILED(native->GetBlobSize(key, &size)) || size == 0) {
                    return;
                }
                std::vector<UINT8> bytes(size);
                UINT32 copied{};
                if (SUCCEEDED(native->GetBlob(
                            key, bytes.data(), size, &copied))
                        && copied == size) {
                    static_cast<void>(
                        type->SetBlob(key, bytes.data(), size));
                }
            };

            UINT32 interlace = 0;
            if (SUCCEEDED(native->GetUINT32(MF_MT_INTERLACE_MODE, &interlace))
                    && interlace != 0) {
                static_cast<void>(
                    type->SetUINT32(MF_MT_INTERLACE_MODE, interlace));
            }
            copy_uint32(kMfMtVideoRotation);
            copy_uint32(MF_MT_VIDEO_PRIMARIES);
            copy_uint32(MF_MT_TRANSFER_FUNCTION);
            copy_uint32(MF_MT_YUV_MATRIX);
            copy_uint32(MF_MT_VIDEO_NOMINAL_RANGE);
            copy_uint32(MF_MT_VIDEO_CHROMA_SITING);
            copy_blob(MF_MT_GEOMETRIC_APERTURE);
            copy_blob(MF_MT_MINIMUM_DISPLAY_APERTURE);
            UINT32 num{}, den{};
            if (SUCCEEDED(MFGetAttributeRatio(
                        native, MF_MT_FRAME_RATE, &num, &den))
                    && num != 0 && den != 0) {
                // External DASH metadata is not trustworthy; a stream can
                // declare an absurd rate. Pinning one of those would be far
                // worse than leaving the rate unset.
                const double fps =
                    static_cast<double>(num) / static_cast<double>(den);
                if (fps >= 1.0 && fps <= 480.0) {
                    static_cast<void>(MFSetAttributeRatio(
                        type, MF_MT_FRAME_RATE, num, den));
                }
            }
            UINT32 frame_w{}, frame_h{};
            const bool has_frame_size = SUCCEEDED(MFGetAttributeSize(
                    native, MF_MT_FRAME_SIZE, &frame_w, &frame_h))
                && frame_w != 0 && frame_h != 0;
            if (config_.output_format == MfD3D11VideoPixelFormat::nv12) {
                // Native NV12 must retain the coded dimensions and source SAR.
                // Display-size normalization belongs to the Cast normalizer;
                // doing it here would force the Source Reader to insert an XVP.
                if (has_frame_size) {
                    static_cast<void>(MFSetAttributeSize(
                        type, MF_MT_FRAME_SIZE, frame_w, frame_h));
                }
                UINT32 par_num{}, par_den{};
                if (SUCCEEDED(MFGetAttributeRatio(native,
                            MF_MT_PIXEL_ASPECT_RATIO,
                            &par_num, &par_den))
                        && par_num != 0 && par_den != 0) {
                    static_cast<void>(MFSetAttributeRatio(type,
                        MF_MT_PIXEL_ASPECT_RATIO, par_num, par_den));
                }
                return type;
            }

            // Square-pixel BGRA output: correct the frame size for the stream's
            // pixel aspect ratio (growing the short axis so no source detail
            // is discarded) and declare the output 1:1. Copying the stream's
            // SAR across was wrong: the reader already size-corrects (e.g.
            // 720x480 @ 8:9 becomes 720x540), so a stale SAR reads back as a
            // different DAR and the processor pads the picture a second time.
            if (has_frame_size) {
                UINT32 par_num{}, par_den{};
                if (SUCCEEDED(MFGetAttributeRatio(native,
                            MF_MT_PIXEL_ASPECT_RATIO,
                            &par_num, &par_den))
                        && par_num != 0 && par_den != 0
                        && par_num != par_den) {
                    if (par_num > par_den) {
                        frame_w = static_cast<UINT32>(
                            (static_cast<std::uint64_t>(frame_w) * par_num
                                + par_den / 2) / par_den);
                    } else {
                        frame_h = static_cast<UINT32>(
                            (static_cast<std::uint64_t>(frame_h) * par_den
                                + par_num / 2) / par_num);
                    }
                }
                static_cast<void>(MFSetAttributeSize(
                    type, MF_MT_FRAME_SIZE, frame_w, frame_h));
            }
            static_cast<void>(
                MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
            return type;
        };

        HRESULT hr = E_FAIL;
        for (const bool pin : {true, false}) {
            auto* type = build(pin);
            if (!type) continue;
            hr = reader_->SetCurrentMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type);
            release(type);
            if (SUCCEEDED(hr)) break;
        }
        release(native);
        if (FAILED(hr)) {
            last_hresult_ = hr;
            last_failure_fatal_ = is_fatal_decoder_hresult(hr);
            log_hresult(config_.output_format
                    == MfD3D11VideoPixelFormat::nv12
                ? "MF D3D decoder: set NV12 output failed"
                : "MF D3D decoder: set ARGB32 output failed", hr);
            return false;
        }
        // The chain only exists once the output type is settled.
        configure_chain_transforms();
        last_hresult_ = S_OK;
        last_failure_fatal_ = false;
        return true;
    }

    [[nodiscard]] bool update_format() {
        IMFMediaType* type{};
        auto hr = reader_->GetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type);
        IMFMediaType* native{};
        static_cast<void>(reader_->GetNativeMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native));
        MfD3D11VideoFormat format;
        format.pixel_format = config_.output_format;
        GUID subtype{};
        if (SUCCEEDED(hr)) hr = type->GetGUID(MF_MT_SUBTYPE, &subtype);
        const auto expected_subtype = config_.output_format
                == MfD3D11VideoPixelFormat::nv12
            ? MFVideoFormat_NV12 : MFVideoFormat_ARGB32;
        if (SUCCEEDED(hr) && !IsEqualGUID(subtype, expected_subtype)) {
            hr = E_UNEXPECTED;
        }
        if (SUCCEEDED(hr)) {
            hr = MFGetAttributeSize(
                type, MF_MT_FRAME_SIZE, &format.width, &format.height);
        }
        UINT32 num{}, den{};
        if (SUCCEEDED(hr) && SUCCEEDED(MFGetAttributeRatio(
                type, MF_MT_PIXEL_ASPECT_RATIO, &num, &den))
                && num != 0 && den != 0) {
            format.pixel_aspect_ratio_num = num;
            format.pixel_aspect_ratio_den = den;
        }
        if (SUCCEEDED(hr) && SUCCEEDED(MFGetAttributeRatio(
                type, MF_MT_FRAME_RATE, &num, &den))
                && num != 0 && den != 0) {
            format.frame_rate_num = num;
            format.frame_rate_den = den;
        }
        const auto optional_uint32 = [&](REFGUID key) {
            UINT32 value{};
            if (FAILED(type->GetUINT32(key, &value)) && native) {
                static_cast<void>(native->GetUINT32(key, &value));
            }
            return value;
        };
        const auto read_aperture = [&](REFGUID key, MFVideoArea& area) {
            UINT32 bytes{};
            if (SUCCEEDED(type->GetBlob(
                       key,
                       reinterpret_cast<UINT8*>(&area),
                       sizeof(area),
                       &bytes))
                && bytes == sizeof(area)
                && area.Area.cx > 0 && area.Area.cy > 0) {
                return true;
            }
            bytes = 0;
            return native && SUCCEEDED(native->GetBlob(
                       key,
                       reinterpret_cast<UINT8*>(&area),
                       sizeof(area),
                       &bytes))
                && bytes == sizeof(area)
                && area.Area.cx > 0 && area.Area.cy > 0;
        };
        if (SUCCEEDED(hr)) {
            format.interlace_mode = optional_uint32(MF_MT_INTERLACE_MODE);
            format.rotation_degrees = optional_uint32(kMfMtVideoRotation);
            format.video_primaries = optional_uint32(MF_MT_VIDEO_PRIMARIES);
            format.transfer_function = optional_uint32(MF_MT_TRANSFER_FUNCTION);
            format.yuv_matrix = optional_uint32(MF_MT_YUV_MATRIX);
            format.nominal_range = optional_uint32(MF_MT_VIDEO_NOMINAL_RANGE);
            format.chroma_siting = optional_uint32(MF_MT_VIDEO_CHROMA_SITING);
            MFVideoArea aperture{};
            if (read_aperture(MF_MT_GEOMETRIC_APERTURE, aperture)
                    || read_aperture(
                        MF_MT_MINIMUM_DISPLAY_APERTURE, aperture)) {
                const auto offset = [](const MFOffset& value) {
                    return static_cast<double>(value.value)
                        + static_cast<double>(value.fract) / 65536.0;
                };
                format.clean_aperture_x = offset(aperture.OffsetX);
                format.clean_aperture_y = offset(aperture.OffsetY);
                format.clean_aperture_width =
                    static_cast<std::uint32_t>(aperture.Area.cx);
                format.clean_aperture_height =
                    static_cast<std::uint32_t>(aperture.Area.cy);
            }
        }
        release(type);
        release(native);
        if (FAILED(hr) || !format.valid()) {
            last_hresult_ = FAILED(hr) ? hr : E_UNEXPECTED;
            last_failure_fatal_ =
                is_fatal_decoder_hresult(last_hresult_);
            log_hresult("MF D3D decoder: read output format failed", hr);
            return false;
        }
        width_ = format.width;
        height_ = format.height;
        format_ = format;
        last_hresult_ = S_OK;
        last_failure_fatal_ = false;
        return true;
    }

    [[nodiscard]] bool seek_internal(double seconds) {
        PROPVARIANT position{};
        InitPropVariantFromInt64(hns_from_seconds(seconds), &position);
        const auto hr = reader_->SetCurrentPosition(GUID_NULL, position);
        PropVariantClear(&position);
        if (FAILED(hr)) {
            last_hresult_ = hr;
            last_failure_fatal_ = false;
            log_hresult("MF D3D decoder: seek failed", hr);
            return false;
        }
        last_hresult_ = S_OK;
        last_failure_fatal_ = false;
        return true;
    }

    [[nodiscard]] DecoderReadStatus read_next_sample(
            IMFSample*& sample, LONGLONG& timestamp, LONGLONG& duration) {
        sample = nullptr;
        timestamp = 0;
        duration = 0;
        for (;;) {
            DWORD stream_index{};
            DWORD flags{};
            auto hr = reader_->ReadSample(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                0,
                &stream_index,
                &flags,
                &timestamp,
                &sample);
            static_cast<void>(stream_index);
            if (FAILED(hr)) {
                last_hresult_ = hr;
                last_failure_fatal_ = is_fatal_decoder_hresult(hr);
                log_hresult("MF D3D decoder: decode failed", hr);
                release(sample);
                return failure_status(hr);
            }
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                // A mid-stream parameter-set change renegotiates the chain.
                // Reapply the selected output policy (pinned BGRA or bare
                // NV12) and disable FRC on any processor that was inserted.
                if (!apply_output_media_type() || !update_format()) {
                    release(sample);
                    return failure_status(last_hresult_);
                }
                ++format_change_count_;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                last_hresult_ = S_OK;
                last_failure_fatal_ = false;
                release(sample);
                return DecoderReadStatus::end_of_stream;
            }
            if (!sample) continue;
            LONGLONG sample_time{};
            if (SUCCEEDED(sample->GetSampleTime(&sample_time))) {
                timestamp = sample_time;
            }
            LONGLONG sample_duration{};
            if (SUCCEEDED(sample->GetSampleDuration(&sample_duration))) {
                duration = sample_duration;
            }
            last_hresult_ = S_OK;
            last_failure_fatal_ = false;
            return DecoderReadStatus::frame;
        }
    }

    [[nodiscard]] std::shared_ptr<const DecodedD3D11VideoFrame>
    texture_from_sample(
            IMFSample* sample,
            LONGLONG timestamp,
            LONGLONG duration,
            std::uint64_t frame_id,
            std::uint64_t generation) {
        IMFMediaBuffer* buffer{};
        IMFDXGIBuffer* dxgi_buffer{};
        ID3D11Texture2D* source_texture{};
        auto hr = sample->GetBufferByIndex(0, &buffer);
        if (SUCCEEDED(hr)) {
            hr = buffer->QueryInterface(IID_PPV_ARGS(&dxgi_buffer));
        }
        if (SUCCEEDED(hr)) {
            hr = dxgi_buffer->GetResource(
                IID_PPV_ARGS(&source_texture));
        }
        UINT source_subresource{};
        if (SUCCEEDED(hr)) {
            hr = dxgi_buffer->GetSubresourceIndex(&source_subresource);
        }
        if (FAILED(hr) || !source_texture) {
            last_hresult_ = FAILED(hr) ? hr : E_NOINTERFACE;
            last_failure_fatal_ =
                is_fatal_decoder_hresult(last_hresult_);
            log_hresult(
                "MF D3D decoder: output is not a DXGI texture", hr);
            release(source_texture);
            release(dxgi_buffer);
            release(buffer);
            return {};
        }

        D3D11_TEXTURE2D_DESC source_desc{};
        source_texture->GetDesc(&source_desc);
        const auto mip = source_desc.MipLevels > 0
            ? source_subresource % source_desc.MipLevels : 0U;
        const auto width = std::max<UINT>(1U, source_desc.Width >> mip);
        const auto height = std::max<UINT>(1U, source_desc.Height >> mip);
        const bool expected_format = config_.output_format
                == MfD3D11VideoPixelFormat::nv12
            ? source_desc.Format == DXGI_FORMAT_NV12
            : (source_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM
                || source_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
        if (!expected_format) {
            last_hresult_ = E_UNEXPECTED;
            last_failure_fatal_ = true;
            log_line(config_.output_format == MfD3D11VideoPixelFormat::nv12
                ? "MF D3D decoder: decoder output is not NV12"
                : "MF D3D decoder: decoder output is not BGRA8");
            release(source_texture);
            release(dxgi_buffer);
            release(buffer);
            return {};
        }

        ID3D11Device* source_device{};
        source_texture->GetDevice(&source_device);
        const bool same_device = source_device == device_;
        release(source_device);
        if (!same_device) {
            last_hresult_ = E_UNEXPECTED;
            last_failure_fatal_ = true;
            log_line("MF D3D decoder: output texture uses a different device");
            release(source_texture);
            release(dxgi_buffer);
            release(buffer);
            return {};
        }

        auto output = acquire_output_texture(
            width, height, config_.output_format);
        auto* output_texture = output.texture;
        if (!output_texture) hr = E_OUTOFMEMORY;
        ID3D11DeviceContext* context{};
        if (SUCCEEDED(hr)) device_->GetImmediateContext(&context);
        if (SUCCEEDED(hr) && context) {
            const auto copy_started = diagnostics_
                ? CastPipelineDiagnostics::qpc_now() : 0;
            context->CopySubresourceRegion(
                output_texture, 0, 0, 0, 0,
                source_texture, source_subresource, nullptr);
            // The Stage compositor uses the same protected immediate context,
            // so command ordering already makes this copy visible before the
            // texture is sampled. Flushing every decoded frame only forces
            // extra driver submissions and can stall the render thread.
            if (diagnostics_) {
                diagnostics_->increment(CastPipelineCounter::gpu_copies);
                diagnostics_->record_duration(
                    CastPipelineEvent::gpu_copy,
                    frame_id,
                    copy_started,
                    CastPipelineDiagnostics::qpc_now(),
                    generation);
            }
        } else if (SUCCEEDED(hr)) {
            hr = E_FAIL;
        }
        release(context);
        release(source_texture);
        release(dxgi_buffer);
        release(buffer);
        if (FAILED(hr) || !output_texture) {
            last_hresult_ = FAILED(hr) ? hr : E_OUTOFMEMORY;
            last_failure_fatal_ =
                is_fatal_decoder_hresult(last_hresult_);
            log_hresult("MF D3D decoder: texture copy failed", hr);
            release(output_texture);
            return {};
        }

        auto frame_format = format_;
        frame_format.width = width;
        frame_format.height = height;
        last_hresult_ = S_OK;
        last_failure_fatal_ = false;
        return std::shared_ptr<const DecodedD3D11VideoFrame>(
            new DecodedD3D11VideoFrame(
                output_texture,
                std::move(output.lease),
                width,
                height,
                seconds_from_hns(timestamp),
                duration > 0
                    ? seconds_from_hns(duration)
                    : kDefaultFrameDurationSeconds,
                frame_id,
                generation,
                frame_format));
    }

    ID3D11Device* device_{};
    MfD3D11VideoDecoderConfig config_;
    IMFDXGIDeviceManager* device_manager_{};
    UINT device_manager_reset_token_{};
    IMFSourceReader* reader_{};
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
