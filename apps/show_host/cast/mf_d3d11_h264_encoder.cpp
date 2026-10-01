#include "mf_d3d11_h264_encoder.h"

#include "cast_nv12_surface_pool.h"
#include "cast_pipeline_diagnostics.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <d3d11.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <objbase.h>

// Current MinGW-w64 ships IMFTrackedSample in evr.h, whose legacy DirectShow
// includes conflict with icodecapi.h, and omits MFCreateTrackedSample. Keep the
// small ABI-compatible declaration local; MSVC's mfidl.h already defines it.
#ifndef __IMFTrackedSample_INTERFACE_DEFINED__
struct IMFTrackedSample : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetAllocator(
        IMFAsyncCallback* sample_allocator,
        IUnknown* state) = 0;
};
#endif

extern "C" HRESULT WINAPI MFCreateTrackedSample(IMFTrackedSample** sample);

namespace {

// Keep less than one 60 fps frame pair queued. If the encoder cannot keep up,
// discard the oldest unencoded input and retain the newest live Stage frame.
constexpr std::size_t kPendingTextureLimit = 2;
constexpr auto kHardwareStartupEventTimeout = std::chrono::milliseconds(500);

template <typename T>
void release(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

struct QueuedMftEvent {
    MediaEventType type{MEUnknown};
    HRESULT status{S_OK};
};

class AsyncMftEventQueue final : public IMFAsyncCallback {
public:
    explicit AsyncMftEventQueue(IMFMediaEventGenerator* generator)
            : generator_(generator) {
        if (generator_) generator_->AddRef();
    }

    HRESULT start() {
        return generator_ ? generator_->BeginGetEvent(this, nullptr)
                          : E_POINTER;
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
    }

    bool try_pop(QueuedMftEvent& event) {
        std::lock_guard lock(mutex_);
        if (events_.empty()) return false;
        event = events_.front();
        events_.pop_front();
        return true;
    }

    template <typename Clock, typename Duration>
    bool wait_until(const std::chrono::time_point<Clock, Duration>& deadline) {
        std::unique_lock lock(mutex_);
        return ready_.wait_until(lock, deadline, [&] {
            return stopping_ || !events_.empty();
        }) && !events_.empty();
    }

    bool wait_for(std::chrono::milliseconds duration) {
        std::unique_lock lock(mutex_);
        return ready_.wait_for(lock, duration, [&] {
            return stopping_ || !events_.empty();
        }) && !events_.empty();
    }

    STDMETHODIMP QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFAsyncCallback)) {
            *value = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    STDMETHODIMP_(ULONG) Release() override {
        const auto remaining = references_.fetch_sub(
            1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) delete this;
        return remaining;
    }

    STDMETHODIMP GetParameters(DWORD*, DWORD*) override {
        return E_NOTIMPL;
    }

    STDMETHODIMP Invoke(IMFAsyncResult* result) override {
        IMFMediaEvent* event{};
        auto status = generator_
            ? generator_->EndGetEvent(result, &event) : E_POINTER;
        QueuedMftEvent queued{};
        queued.status = status;
        if (SUCCEEDED(status) && event) {
            static_cast<void>(event->GetType(&queued.type));
            static_cast<void>(event->GetStatus(&queued.status));
        }
        release(event);

        bool rearm{};
        {
            std::lock_guard lock(mutex_);
            if (!stopping_) {
                events_.push_back(queued);
                rearm = SUCCEEDED(queued.status);
            }
        }
        ready_.notify_all();

        if (rearm) {
            const auto begin_result = generator_->BeginGetEvent(this, nullptr);
            if (FAILED(begin_result)) {
                std::lock_guard lock(mutex_);
                events_.push_back(QueuedMftEvent{MEUnknown, begin_result});
                ready_.notify_all();
            }
        }
        return S_OK;
    }

private:
    ~AsyncMftEventQueue() { release(generator_); }

    std::atomic<ULONG> references_{1};
    IMFMediaEventGenerator* generator_{};
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<QueuedMftEvent> events_;
    bool stopping_{};
};

bool valid_config(const MfD3D11H264EncoderConfig& config) {
    return config.width > 0 && config.height > 0
        && (config.width & 1U) == 0 && (config.height & 1U) == 0
        && config.frame_rate_num > 0 && config.frame_rate_den > 0
        && config.bitrate > 0 && config.gop_size_frames > 0;
}

void set_optional_uint32_codec_property(
        IMFTransform* encoder, const GUID& property, std::uint32_t value) {
    ICodecAPI* codec_api{};
    if (!encoder
            || FAILED(encoder->QueryInterface(IID_PPV_ARGS(&codec_api)))) {
        return;
    }
    VARIANT setting{};
    VariantInit(&setting);
    setting.vt = VT_UI4;
    setting.ulVal = value;
    static_cast<void>(codec_api->SetValue(&property, &setting));
    VariantClear(&setting);
    release(codec_api);
}

HRESULT set_force_keyframe(ICodecAPI* codec_api) {
    if (!codec_api) return E_NOINTERFACE;
    VARIANT setting{};
    VariantInit(&setting);
    setting.vt = VT_UI4;
    setting.ulVal = 1;
    const auto result = codec_api->SetValue(
        &CODECAPI_AVEncVideoForceKeyFrame, &setting);
    VariantClear(&setting);
    return result;
}

IMFMediaType* make_video_type(
        const GUID& subtype,
        const MfD3D11H264EncoderConfig& config,
        bool compressed) {
    IMFMediaType* type{};
    auto hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(hr)) {
        hr = MFSetAttributeSize(
            type, MF_MT_FRAME_SIZE, config.width, config.height);
    }
    if (SUCCEEDED(hr)) {
        hr = MFSetAttributeRatio(
            type,
            MF_MT_FRAME_RATE,
            config.frame_rate_num,
            config.frame_rate_den);
    }
    if (SUCCEEDED(hr)) {
        hr = MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(
            MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    }
    if (SUCCEEDED(hr) && compressed) {
        hr = type->SetUINT32(MF_MT_AVG_BITRATE, config.bitrate);
    }
    if (FAILED(hr)) release(type);
    return type;
}

class LeaseRetentionCallback final : public IMFAsyncCallback {
public:
    explicit LeaseRetentionCallback(
            std::shared_ptr<void> retention,
            std::shared_ptr<std::atomic_bool> released = {}) noexcept
        : retention_(std::move(retention)), released_(std::move(released)) {}

    STDMETHODIMP QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFAsyncCallback)) {
            *value = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    STDMETHODIMP_(ULONG) Release() override {
        const auto remaining = references_.fetch_sub(
            1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) delete this;
        return remaining;
    }

    STDMETHODIMP GetParameters(DWORD*, DWORD*) override {
        return E_NOTIMPL;
    }

    STDMETHODIMP Invoke(IMFAsyncResult*) override {
        retention_.reset();
        if (released_) released_->store(true, std::memory_order_release);
        return S_OK;
    }

private:
    ~LeaseRetentionCallback() = default;
    std::atomic<ULONG> references_{1};
    std::shared_ptr<void> retention_;
    std::shared_ptr<std::atomic_bool> released_;
};

IMFSample* make_dxgi_sample(
        ID3D11Texture2D* texture,
        LONGLONG timestamp,
        LONGLONG duration,
        bool discontinuity,
        std::shared_ptr<void> retention,
        std::shared_ptr<std::atomic_bool> released = {}) {
    IMFMediaBuffer* buffer{};
    auto hr = MFCreateDXGISurfaceBuffer(
        __uuidof(ID3D11Texture2D), texture, 0, FALSE, &buffer);
    IMFSample* sample{};
    IMFTrackedSample* tracked{};
    LeaseRetentionCallback* callback{};
    if (SUCCEEDED(hr) && retention) {
        hr = MFCreateTrackedSample(&tracked);
        if (SUCCEEDED(hr)) {
            hr = tracked->QueryInterface(IID_PPV_ARGS(&sample));
        }
        if (SUCCEEDED(hr)) {
            callback = new (std::nothrow) LeaseRetentionCallback(
                std::move(retention), std::move(released));
            if (!callback) hr = E_OUTOFMEMORY;
        }
        if (SUCCEEDED(hr)) hr = tracked->SetAllocator(callback, nullptr);
    } else if (SUCCEEDED(hr)) {
        hr = MFCreateSample(&sample);
    }
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(timestamp);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(duration);
    if (SUCCEEDED(hr) && discontinuity) {
        hr = sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
    }
    release(callback);
    release(tracked);
    release(buffer);
    if (FAILED(hr)) release(sample);
    return sample;
}

IMFSample* make_memory_sample(
        const std::vector<std::uint8_t>& bytes,
        LONGLONG timestamp,
        LONGLONG duration,
        bool discontinuity,
        DWORD alignment = 16) {
    if (bytes.empty() || bytes.size() > MAXDWORD) return nullptr;
    IMFMediaBuffer* buffer{};
    alignment = std::max<DWORD>(alignment, 16);
    auto hr = MFCreateAlignedMemoryBuffer(
        static_cast<DWORD>(bytes.size()), alignment - 1, &buffer);
    BYTE* destination{};
    DWORD capacity{};
    if (SUCCEEDED(hr)) hr = buffer->Lock(&destination, &capacity, nullptr);
    if (SUCCEEDED(hr) && capacity >= bytes.size()) {
        std::memcpy(destination, bytes.data(), bytes.size());
    } else if (SUCCEEDED(hr)) {
        hr = E_UNEXPECTED;
    }
    if (destination) buffer->Unlock();
    if (SUCCEEDED(hr)) {
        hr = buffer->SetCurrentLength(static_cast<DWORD>(bytes.size()));
    }

    IMFSample* sample{};
    if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(timestamp);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(duration);
    if (SUCCEEDED(hr) && discontinuity) {
        hr = sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
    }
    release(buffer);
    if (FAILED(hr)) release(sample);
    return sample;
}

class D3D11Nv12Readback {
public:
    ~D3D11Nv12Readback() { reset(); }

    bool configure(ID3D11Device* device,
            std::uint32_t width, std::uint32_t height) {
        reset();
        if (!device || width == 0 || height == 0) return false;
        device->GetImmediateContext(&context_);
        if (!context_) return false;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging_))) {
            reset();
            return false;
        }
        width_ = width;
        height_ = height;
        return true;
    }

    bool read(ID3D11Texture2D* source, std::vector<std::uint8_t>& output) {
        if (!source || !context_ || !staging_) return false;
        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        if (source_desc.Width != width_ || source_desc.Height != height_
                || source_desc.Format != DXGI_FORMAT_NV12) {
            return false;
        }

        context_->CopyResource(staging_, source);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(
                staging_, 0, D3D11_MAP_READ, 0, &mapped))) {
            return false;
        }

        const auto y_bytes = static_cast<std::size_t>(width_) * height_;
        const auto uv_bytes = y_bytes / 2U;
        output.resize(y_bytes + uv_bytes);
        const auto* source_bytes = static_cast<const std::uint8_t*>(mapped.pData);
        for (std::uint32_t row = 0; row < height_; ++row) {
            std::memcpy(
                output.data() + static_cast<std::size_t>(row) * width_,
                source_bytes + static_cast<std::size_t>(row) * mapped.RowPitch,
                width_);
        }
        const auto* source_uv = source_bytes
            + static_cast<std::size_t>(mapped.RowPitch) * height_;
        auto* destination_uv = output.data() + y_bytes;
        for (std::uint32_t row = 0; row < height_ / 2U; ++row) {
            std::memcpy(
                destination_uv + static_cast<std::size_t>(row) * width_,
                source_uv + static_cast<std::size_t>(row) * mapped.RowPitch,
                width_);
        }
        context_->Unmap(staging_, 0);
        return true;
    }

private:
    void reset() {
        release(staging_);
        release(context_);
        width_ = 0;
        height_ = 0;
    }

    ID3D11DeviceContext* context_{};
    ID3D11Texture2D* staging_{};
    std::uint32_t width_{};
    std::uint32_t height_{};
};

HRESULT renegotiate_h264_output(
        IMFTransform* encoder,
        const MfD3D11H264EncoderConfig& config) {
    if (!encoder) return E_POINTER;
    HRESULT last_result = MF_E_INVALIDMEDIATYPE;
    for (DWORD index = 0;; ++index) {
        IMFMediaType* type{};
        const auto available_result = encoder->GetOutputAvailableType(
            0, index, &type);
        if (available_result == MF_E_NO_MORE_TYPES) break;
        if (FAILED(available_result)) return available_result;

        GUID subtype{};
        const bool h264 = SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype))
            && subtype == MFVideoFormat_H264;
        if (h264) {
            static_cast<void>(type->SetUINT32(
                MF_MT_AVG_BITRATE, config.bitrate));
            static_cast<void>(MFSetAttributeSize(
                type, MF_MT_FRAME_SIZE, config.width, config.height));
            static_cast<void>(MFSetAttributeRatio(
                type,
                MF_MT_FRAME_RATE,
                config.frame_rate_num,
                config.frame_rate_den));
            last_result = encoder->SetOutputType(0, type, 0);
        }
        release(type);
        if (h264 && SUCCEEDED(last_result)) return S_OK;
    }
    return last_result;
}

class ReusableMftOutputSample {
public:
    ReusableMftOutputSample() = default;
    ~ReusableMftOutputSample() { reset(); }

    ReusableMftOutputSample(const ReusableMftOutputSample&) = delete;
    ReusableMftOutputSample& operator=(
        const ReusableMftOutputSample&) = delete;

    HRESULT prepare(
            const MFT_OUTPUT_STREAM_INFO& info,
            IMFSample** sample) {
        if (!sample) return E_POINTER;
        *sample = nullptr;
        if ((info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0) {
            reset();
            return S_OK;
        }

        const auto required_capacity = std::max<DWORD>(
            info.cbSize, 2U * 1024U * 1024U);
        if (!sample_ || !buffer_ || capacity_ < required_capacity) {
            reset();
            auto hr = MFCreateSample(&sample_);
            if (SUCCEEDED(hr)) {
                hr = MFCreateMemoryBuffer(required_capacity, &buffer_);
            }
            if (SUCCEEDED(hr)) hr = sample_->AddBuffer(buffer_);
            if (FAILED(hr)) {
                reset();
                return hr;
            }
            capacity_ = required_capacity;
        }

        auto hr = sample_->DeleteAllItems();
        if (SUCCEEDED(hr)) hr = buffer_->SetCurrentLength(0);
        if (SUCCEEDED(hr)) *sample = sample_;
        return hr;
    }

private:
    void reset() noexcept {
        release(buffer_);
        release(sample_);
        capacity_ = 0;
    }

    IMFSample* sample_{};
    IMFMediaBuffer* buffer_{};
    DWORD capacity_{};
};

template <typename Consumer>
HRESULT consume_encoded_output(
        IMFTransform* encoder,
        ReusableMftOutputSample& reusable_sample,
        Consumer&& consumer) {
    MFT_OUTPUT_STREAM_INFO info{};
    auto hr = encoder->GetOutputStreamInfo(0, &info);
    if (FAILED(hr)) return hr;

    IMFSample* caller_sample{};
    hr = reusable_sample.prepare(info, &caller_sample);
    if (FAILED(hr)) return hr;

    MFT_OUTPUT_DATA_BUFFER output{};
    output.dwStreamID = 0;
    output.pSample = caller_sample;
    DWORD status{};
    hr = encoder->ProcessOutput(0, 1, &output, &status);
    static_cast<void>(status);
    if (FAILED(hr)) {
        release(output.pEvents);
        if (output.pSample != caller_sample) release(output.pSample);
        return hr;
    }

    IMFMediaBuffer* contiguous{};
    hr = output.pSample
        ? output.pSample->ConvertToContiguousBuffer(&contiguous) : E_FAIL;
    BYTE* data{};
    DWORD byte_count{};
    if (SUCCEEDED(hr)) hr = contiguous->Lock(&data, nullptr, &byte_count);
    if (SUCCEEDED(hr) && byte_count > 0) {
        LONGLONG timestamp{};
        std::int64_t pts90k{};
        if (SUCCEEDED(output.pSample->GetSampleTime(&timestamp))) {
            pts90k = timestamp * 9LL / 1000LL;
        }
        UINT32 clean{};
        const bool keyframe = SUCCEEDED(output.pSample->GetUINT32(
            MFSampleExtension_CleanPoint, &clean)) && clean != 0;
        consumer(data, static_cast<std::size_t>(byte_count),
            pts90k, keyframe);
    }
    if (data) contiguous->Unlock();
    release(contiguous);
    release(output.pEvents);
    if (output.pSample != caller_sample) release(output.pSample);
    return SUCCEEDED(hr) && byte_count > 0 ? S_OK : E_FAIL;
}

struct PendingTexture {
    ID3D11Texture2D* texture{};
    std::shared_ptr<void> retention;
    LONGLONG timestamp{};
    LONGLONG duration{};
    bool discontinuity{};
    bool force_keyframe{};
};

void release_pending(PendingTexture& pending) {
    pending.retention.reset();
    pending = {};
}

}  // namespace

struct MfD3D11H264Encoder::Impl {
    mutable std::mutex submission_mutex;
    mutable std::mutex mutex;
    std::condition_variable state_changed;
    std::condition_variable startup_ready;
    std::thread worker;
    ID3D11Device* device{};
    MfD3D11H264EncoderConfig config;
    OutputCallback output;
    D3D11BgraToNv12Converter converter;
    CastNv12SurfacePool surface_pool;
    CastPipelineDiagnostics* diagnostics{};
    std::deque<PendingTexture> pending;
    ID3D11Texture2D* cached_bgra_texture{};
    ID3D11Texture2D* cached_nv12_texture{};
    std::shared_ptr<void> cached_nv12_retention;
    ID3D11Texture2D* startup_nv12_texture{};
    std::shared_ptr<void> startup_nv12_retention;
    bool surface_pool_needs_copy{};
    bool stop_requested{};
    bool startup_complete{};
    bool startup_succeeded{};
    bool startup_retained_async{};
    bool running{};
    bool first_input{true};
    MfH264EncoderBackend active_backend{MfH264EncoderBackend::none};
    MfH264ForceKeyframeSupport keyframe_support{
        MfH264ForceKeyframeSupport::unknown};

    ~Impl() { stop(); }

    void clear_pending_locked() {
        for (auto& texture : pending) release_pending(texture);
        pending.clear();
    }

    void clear_startup_sample_locked() {
        release(startup_nv12_texture);
        startup_nv12_retention.reset();
    }

    void clear_conversion_cache_locked() {
        release(cached_bgra_texture);
        cached_nv12_texture = nullptr;
        cached_nv12_retention.reset();
    }

    void cache_conversion_locked(
            ID3D11Texture2D* bgra,
            ID3D11Texture2D* nv12,
            std::shared_ptr<void> retention) {
        clear_conversion_cache_locked();
        if (!bgra || !nv12 || !retention) return;
        bgra->AddRef();
        cached_bgra_texture = bgra;
        cached_nv12_texture = nv12;
        cached_nv12_retention = std::move(retention);
    }

    void enqueue_locked(
            ID3D11Texture2D* nv12,
            std::shared_ptr<void> retention,
            std::int64_t timestamp,
            std::int64_t duration,
            bool& dropped_stale_input,
            bool force_keyframe) {
        bool dropped_keyframe_request{};
        while (pending.size() >= kPendingTextureLimit) {
            dropped_keyframe_request = dropped_keyframe_request
                || pending.front().force_keyframe;
            release_pending(pending.front());
            pending.pop_front();
            dropped_stale_input = true;
            if (diagnostics) {
                diagnostics->increment(
                    CastPipelineCounter::encoder_drops);
            }
        }
        // Preserve a program-switch request when backpressure discards the
        // frame that carried it. The earliest surviving input gets the bit;
        // this keeps the request tied to a real queued frame and avoids a
        // process-wide flag accidentally targeting an older hold frame.
        if (dropped_keyframe_request) {
            if (!pending.empty()) {
                pending.front().force_keyframe = true;
            } else {
                force_keyframe = true;
            }
        }
        pending.push_back(PendingTexture{
            nv12,
            std::move(retention),
            static_cast<LONGLONG>(timestamp),
            static_cast<LONGLONG>(duration),
            first_input || dropped_stale_input,
            force_keyframe});
        first_input = false;
    }

    bool start(
            void* borrowed_device,
            const MfD3D11H264EncoderConfig& requested_config,
            OutputCallback requested_output,
            CastPipelineDiagnostics* requested_diagnostics,
            void* borrowed_startup_nv12_texture,
            std::shared_ptr<void> requested_startup_retention) {
        auto* requested_device = static_cast<ID3D11Device*>(borrowed_device);
        auto* requested_startup_texture =
            static_cast<ID3D11Texture2D*>(borrowed_startup_nv12_texture);
        if (!requested_device || !valid_config(requested_config)
                || !requested_output) {
            return false;
        }
        if ((requested_startup_texture != nullptr)
                != static_cast<bool>(requested_startup_retention)) {
            return false;
        }
        if (requested_startup_texture
                && requested_config.input_format
                    != MfH264EncoderInputFormat::nv12) {
            return false;
        }
        std::unique_lock submission_lock(submission_mutex);
        std::unique_lock lock(mutex);
        if (worker.joinable()) return running && device == requested_device;
        surface_pool_needs_copy = false;
        if (requested_config.input_format
                == MfH264EncoderInputFormat::bgra8) {
            if (!converter.configure(
                    requested_device,
                    requested_config.width,
                    requested_config.height,
                    requested_config.frame_rate_num,
                    requested_config.frame_rate_den,
                    requested_diagnostics)) {
                return false;
            }
            CastNv12SurfacePoolConfig pool_config;
            pool_config.width = requested_config.width;
            pool_config.height = requested_config.height;
            pool_config.capacity = 8;
            pool_config.create_plane_uavs = false;
            pool_config.video_processor_output = true;
            pool_config.encoder_compatible = true;
            pool_config.separate_encoder_texture = false;
            if (!surface_pool.configure(
                    requested_device, pool_config, requested_diagnostics)) {
                pool_config.separate_encoder_texture = true;
                if (!surface_pool.configure(
                        requested_device,
                        pool_config,
                        requested_diagnostics)) {
                    converter.reset();
                    return false;
                }
            }
            surface_pool_needs_copy = pool_config.separate_encoder_texture;
            for (std::size_t index = 0;
                    index < surface_pool.capacity(); ++index) {
                if (!converter.prepare_output_texture(
                        surface_pool.compose_texture_at(index))) {
                    converter.reset();
                    surface_pool.reset();
                    return false;
                }
            }
        }
        requested_device->AddRef();
        device = requested_device;
        config = requested_config;
        output = std::move(requested_output);
        diagnostics = requested_diagnostics;
        if (requested_startup_texture) requested_startup_texture->AddRef();
        startup_nv12_texture = requested_startup_texture;
        startup_nv12_retention = std::move(requested_startup_retention);
        stop_requested = false;
        startup_complete = false;
        startup_succeeded = false;
        startup_retained_async = false;
        running = false;
        first_input = true;
        active_backend = MfH264EncoderBackend::none;
        keyframe_support = MfH264ForceKeyframeSupport::unknown;
        clear_pending_locked();
        clear_conversion_cache_locked();
        try {
            worker = std::thread([this] { worker_main(); });
        } catch (...) {
            release(device);
            converter.reset();
            surface_pool.reset();
            clear_startup_sample_locked();
            output = {};
            diagnostics = nullptr;
            return false;
        }

        startup_ready.wait(lock, [&] { return startup_complete; });
        if (startup_succeeded) return true;
        stop_requested = true;
        lock.unlock();
        state_changed.notify_all();
        worker.join();
        lock.lock();
        release(device);
        converter.reset();
        surface_pool.reset();
        clear_startup_sample_locked();
        output = {};
        diagnostics = nullptr;
        return false;
    }

    void stop() {
        std::lock_guard submission_lock(submission_mutex);
        {
            std::lock_guard lock(mutex);
            if (!worker.joinable()) {
                clear_pending_locked();
                clear_conversion_cache_locked();
                release(device);
                converter.reset();
                surface_pool.reset();
                clear_startup_sample_locked();
                output = {};
                diagnostics = nullptr;
                running = false;
                active_backend = MfH264EncoderBackend::none;
                keyframe_support = MfH264ForceKeyframeSupport::unknown;
                return;
            }
            stop_requested = true;
        }
        state_changed.notify_all();
        worker.join();
        std::lock_guard lock(mutex);
        clear_pending_locked();
        clear_conversion_cache_locked();
        release(device);
        converter.reset();
        surface_pool.reset();
        clear_startup_sample_locked();
        output = {};
        diagnostics = nullptr;
        running = false;
        active_backend = MfH264EncoderBackend::none;
        keyframe_support = MfH264ForceKeyframeSupport::unknown;
        startup_complete = false;
        startup_succeeded = false;
        startup_retained_async = false;
        stop_requested = false;
    }

    bool is_running() const {
        std::lock_guard lock(mutex);
        return running && !stop_requested;
    }

    MfH264EncoderBackend backend() const {
        std::lock_guard lock(mutex);
        return running && !stop_requested
            ? active_backend : MfH264EncoderBackend::none;
    }

    MfH264ForceKeyframeSupport force_keyframe_support() const {
        std::lock_guard lock(mutex);
        return running && !stop_requested
            ? keyframe_support : MfH264ForceKeyframeSupport::unknown;
    }

    bool startup_sample_retained_async() const {
        std::lock_guard lock(mutex);
        return running && !stop_requested && startup_retained_async;
    }

    bool prepare_input_texture(void* composed_bgra_texture) {
        if (!composed_bgra_texture) return false;
        std::lock_guard submission_lock(submission_mutex);
        {
            std::lock_guard lock(mutex);
            if (!running || stop_requested
                    || config.input_format
                        != MfH264EncoderInputFormat::bgra8) {
                return false;
            }
        }
        return converter.prepare_input_texture(composed_bgra_texture);
    }

    bool submit_texture(
            void* composed_bgra_texture,
            std::int64_t timestamp,
            std::int64_t duration,
            bool force_keyframe) {
        if (!composed_bgra_texture || timestamp < 0 || duration <= 0) {
            return false;
        }
        std::lock_guard submission_lock(submission_mutex);
        bool dropped_stale_input = false;
        {
            std::lock_guard lock(mutex);
            if (!running || stop_requested
                    || config.input_format
                        != MfH264EncoderInputFormat::bgra8) {
                return false;
            }
        }

        auto surface = surface_pool.try_acquire();
        if (!surface) {
            if (diagnostics) {
                diagnostics->increment(CastPipelineCounter::encoder_drops);
            }
            return false;
        }
        auto* compose_nv12 = static_cast<ID3D11Texture2D*>(
            surface.compose_texture());
        auto* encoder_nv12 = static_cast<ID3D11Texture2D*>(
            surface.encoder_texture());
        const auto correlation_id = duration > 0
            ? static_cast<std::uint64_t>(timestamp / duration) : 0;
        if (!converter.convert(
                composed_bgra_texture, compose_nv12, correlation_id)) {
            return false;
        }
        if (surface_pool_needs_copy) {
            ID3D11DeviceContext* context{};
            device->GetImmediateContext(&context);
            if (!context) return false;
            const auto copy_started = diagnostics
                ? CastPipelineDiagnostics::qpc_now() : 0;
            context->CopyResource(encoder_nv12, compose_nv12);
            release(context);
            if (diagnostics) {
                diagnostics->increment(CastPipelineCounter::gpu_copies);
                diagnostics->record_duration(
                    CastPipelineEvent::gpu_copy,
                    correlation_id,
                    copy_started,
                    CastPipelineDiagnostics::qpc_now(),
                    static_cast<std::uint64_t>(timestamp));
            }
        }
        auto retention = surface.retention_token();
        surface.reset();

        {
            std::lock_guard lock(mutex);
            if (!running || stop_requested) {
                return false;
            }
            cache_conversion_locked(
                static_cast<ID3D11Texture2D*>(composed_bgra_texture),
                encoder_nv12,
                retention);
            enqueue_locked(
                encoder_nv12,
                std::move(retention),
                timestamp,
                duration,
                dropped_stale_input,
                force_keyframe);
        }
        state_changed.notify_one();
        return true;
    }

    bool submit_repeated_texture(
            void* unchanged_bgra_texture,
            std::int64_t timestamp,
            std::int64_t duration,
            bool force_keyframe) {
        auto* bgra = static_cast<ID3D11Texture2D*>(
            unchanged_bgra_texture);
        if (!bgra || timestamp < 0 || duration <= 0) return false;
        std::unique_lock submission_lock(submission_mutex);

        bool reused{};
        bool dropped_stale_input{};
        {
            std::lock_guard lock(mutex);
            if (!running || stop_requested
                    || config.input_format
                        != MfH264EncoderInputFormat::bgra8) {
                return false;
            }
            if (cached_bgra_texture == bgra && cached_nv12_texture
                    && cached_nv12_retention) {
                enqueue_locked(
                    cached_nv12_texture,
                    cached_nv12_retention,
                    timestamp, duration, dropped_stale_input,
                    force_keyframe);
                reused = true;
                if (diagnostics) {
                    diagnostics->increment(
                        CastPipelineCounter::repeated_frames);
                }
            }
        }
        if (!reused) {
            submission_lock.unlock();
            return submit_texture(
                bgra, timestamp, duration, force_keyframe);
        }
        state_changed.notify_one();
        return true;
    }

    bool submit_nv12_texture(
            void* borrowed_nv12_texture,
            std::shared_ptr<void> retention,
            std::int64_t timestamp,
            std::int64_t duration,
            bool force_keyframe) {
        auto* nv12 = static_cast<ID3D11Texture2D*>(borrowed_nv12_texture);
        if (!nv12 || !retention || timestamp < 0 || duration <= 0) {
            return false;
        }
        std::lock_guard submission_lock(submission_mutex);
        {
            std::lock_guard lock(mutex);
            if (!running || stop_requested
                    || config.input_format
                        != MfH264EncoderInputFormat::nv12) {
                return false;
            }
        }

        D3D11_TEXTURE2D_DESC desc{};
        nv12->GetDesc(&desc);
        ID3D11Device* texture_device{};
        nv12->GetDevice(&texture_device);
        const bool valid = texture_device == device
            && desc.Format == DXGI_FORMAT_NV12
            && desc.Width == config.width
            && desc.Height == config.height
            && desc.SampleDesc.Count == 1;
        release(texture_device);
        if (!valid) return false;

        bool dropped_stale_input = false;
        {
            std::lock_guard lock(mutex);
            if (!running || stop_requested) return false;
            enqueue_locked(nv12, std::move(retention), timestamp, duration,
                dropped_stale_input, force_keyframe);
        }
        state_changed.notify_one();
        return true;
    }

    static bool configure_stream_types(
            IMFTransform* encoder,
            const MfD3D11H264EncoderConfig& config) {
        if (!encoder) return false;
        auto* output_type = make_video_type(
            MFVideoFormat_H264, config, true);
        auto* input_type = make_video_type(
            MFVideoFormat_NV12, config, false);
        auto hr = output_type
            ? encoder->SetOutputType(0, output_type, 0) : E_OUTOFMEMORY;
        if (SUCCEEDED(hr)) {
            hr = input_type
                ? encoder->SetInputType(0, input_type, 0) : E_OUTOFMEMORY;
        }
        if (SUCCEEDED(hr)) {
            set_optional_uint32_codec_property(
                encoder, CODECAPI_AVLowLatencyMode, 1);
            set_optional_uint32_codec_property(
                encoder,
                CODECAPI_AVEncCommonRateControlMode,
                eAVEncCommonRateControlMode_CBR);
            set_optional_uint32_codec_property(
                encoder, CODECAPI_AVEncCommonMeanBitRate, config.bitrate);
            set_optional_uint32_codec_property(
                encoder, CODECAPI_AVEncMPVGOPSize, config.gop_size_frames);
            set_optional_uint32_codec_property(
                encoder, CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        }
        release(input_type);
        release(output_type);
        return SUCCEEDED(hr);
    }

    static bool configure_hardware_transform(
            IMFTransform* encoder,
            IMFDXGIDeviceManager* device_manager,
            const MfD3D11H264EncoderConfig& config,
            bool use_dxgi_surface_input,
            IMFMediaEventGenerator** event_generator) {
        if (!encoder || !event_generator
                || (use_dxgi_surface_input && !device_manager)) {
            return false;
        }
        *event_generator = nullptr;

        IMFAttributes* attributes{};
        auto hr = encoder->GetAttributes(&attributes);
        UINT32 asynchronous{};
        UINT32 d3d11_aware{};
        if (SUCCEEDED(hr)) {
            static_cast<void>(attributes->GetUINT32(
                MF_TRANSFORM_ASYNC, &asynchronous));
            static_cast<void>(attributes->GetUINT32(
                MF_SA_D3D11_AWARE, &d3d11_aware));
            if (asynchronous) {
                hr = attributes->SetUINT32(
                    MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            }
        }
        if (SUCCEEDED(hr) && (!asynchronous
                || (use_dxgi_surface_input && !d3d11_aware))) {
            hr = E_FAIL;
        }
        if (SUCCEEDED(hr) && use_dxgi_surface_input) {
            hr = encoder->ProcessMessage(
                MFT_MESSAGE_SET_D3D_MANAGER,
                reinterpret_cast<ULONG_PTR>(device_manager));
        }
        if (SUCCEEDED(hr) && !configure_stream_types(encoder, config)) {
            hr = E_FAIL;
        }
        if (SUCCEEDED(hr)) {
            hr = encoder->QueryInterface(IID_PPV_ARGS(event_generator));
        }
        release(attributes);
        return SUCCEEDED(hr) && *event_generator;
    }

    static bool configure_software_transform(
            IMFTransform* encoder,
            const MfD3D11H264EncoderConfig& config) {
        if (!encoder) return false;
        IMFAttributes* attributes{};
        auto hr = encoder->GetAttributes(&attributes);
        UINT32 asynchronous{};
        if (SUCCEEDED(hr)) {
            static_cast<void>(attributes->GetUINT32(
                MF_TRANSFORM_ASYNC, &asynchronous));
        }
        release(attributes);
        return SUCCEEDED(hr) && !asynchronous
            && configure_stream_types(encoder, config);
    }

    std::int64_t prepare_input_diagnostic_flags(
            const PendingTexture& input,
            ICodecAPI* codec_api) {
        auto flags = input.discontinuity
            ? cast_encoder_input_diagnostic::discontinuity : 0;
        if (!input.force_keyframe) return flags;

        flags |= cast_encoder_input_diagnostic::keyframe_requested;
        const auto result = set_force_keyframe(codec_api);
        MfH264ForceKeyframeSupport support{};
        if (!codec_api) {
            support = MfH264ForceKeyframeSupport::unsupported;
            flags |= cast_encoder_input_diagnostic::keyframe_unsupported;
        } else if (SUCCEEDED(result)) {
            support = MfH264ForceKeyframeSupport::supported;
            flags |= cast_encoder_input_diagnostic::keyframe_applied;
        } else {
            support = MfH264ForceKeyframeSupport::failed;
            flags |= cast_encoder_input_diagnostic::keyframe_failed;
        }
        {
            std::lock_guard lock(mutex);
            keyframe_support = support;
        }
        return flags;
    }

    bool activate_hardware_encoder(
            IMFDXGIDeviceManager* device_manager,
            bool readback_input,
            IMFTransform** encoder,
            IMFMediaEventGenerator** event_generator,
            bool* uses_readback_input) {
        if (!encoder || !event_generator || !uses_readback_input) {
            return false;
        }
        *encoder = nullptr;
        *event_generator = nullptr;
        *uses_readback_input = false;
        MFT_REGISTER_TYPE_INFO input_info{
            MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO output_info{
            MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** activations{};
        UINT32 activation_count{};
        auto hr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            &input_info,
            &output_info,
            &activations,
            &activation_count);
        if (FAILED(hr)) return false;

        for (UINT32 i = 0; i < activation_count && !*encoder; ++i) {
            IMFTransform* candidate{};
            hr = activations[i]->ActivateObject(IID_PPV_ARGS(&candidate));
            IMFMediaEventGenerator* candidate_events{};
            if (SUCCEEDED(hr) && configure_hardware_transform(
                    candidate,
                    device_manager,
                    config,
                    !readback_input,
                    &candidate_events)) {
                *encoder = candidate;
                *event_generator = candidate_events;
                *uses_readback_input = readback_input;
            } else {
                release(candidate_events);
                release(candidate);
            }
        }
        for (UINT32 i = 0; i < activation_count; ++i) {
            release(activations[i]);
        }
        CoTaskMemFree(activations);
        return *encoder && *event_generator;
    }

    bool activate_software_encoder(IMFTransform** encoder) {
        if (!encoder) return false;
        *encoder = nullptr;
        MFT_REGISTER_TYPE_INFO input_info{
            MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO output_info{
            MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** activations{};
        UINT32 activation_count{};
        auto hr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            &input_info,
            &output_info,
            &activations,
            &activation_count);
        if (FAILED(hr)) return false;

        for (UINT32 i = 0; i < activation_count && !*encoder; ++i) {
            IMFTransform* candidate{};
            hr = activations[i]->ActivateObject(IID_PPV_ARGS(&candidate));
            if (SUCCEEDED(hr)
                    && configure_software_transform(candidate, config)) {
                *encoder = candidate;
            } else {
                release(candidate);
            }
        }
        for (UINT32 i = 0; i < activation_count; ++i) {
            release(activations[i]);
        }
        CoTaskMemFree(activations);
        return *encoder != nullptr;
    }

    void emit_access_unit(
            const std::uint8_t* data,
            std::size_t size,
            std::int64_t pts90k,
            bool keyframe) {
        OutputCallback callback;
        {
            std::lock_guard lock(mutex);
            callback = output;
        }
        if (callback) {
            callback(data, size, pts90k, keyframe);
        }
        if (diagnostics) {
            // Input timestamps are generated in 100 ns units and then scaled
            // to 90 kHz by the MFT. Dividing by a separately truncated frame
            // duration makes a valid frame N appear as N-1. Recover the
            // transport-frame correlation from the exact rational rate and
            // round to the nearest frame instead.
            const auto non_negative_pts =
                std::max<std::int64_t>(0, pts90k);
            const auto correlation_id = static_cast<std::uint64_t>(
                std::llround(
                    static_cast<double>(non_negative_pts)
                        * config.frame_rate_num
                    / (90000.0 * config.frame_rate_den)));
            diagnostics->record(
                CastPipelineEvent::encoder_output,
                correlation_id,
                static_cast<std::uint64_t>(
                    non_negative_pts),
                keyframe
                    ? -static_cast<std::int64_t>(size)
                    : static_cast<std::int64_t>(size));
        }
    }

    HRESULT drain_software_output(
            IMFTransform* encoder,
            ReusableMftOutputSample& output_sample) {
        for (;;) {
            const auto result = consume_encoded_output(
                encoder,
                output_sample,
                [this](const std::uint8_t* data,
                        std::size_t size,
                        std::int64_t pts90k,
                        bool keyframe) {
                    emit_access_unit(data, size, pts90k, keyframe);
                });
            if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
            if (FAILED(result)) return result;
        }
    }

    void worker_main() {
        const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMFDXGIDeviceManager* device_manager{};
        IMFTransform* encoder{};
        IMFMediaEventGenerator* events{};
        ICodecAPI* codec_api{};
        AsyncMftEventQueue* event_queue{};
        MfH264EncoderBackend selected_backend{MfH264EncoderBackend::none};
        bool hardware_uses_readback{};
        D3D11Nv12Readback nv12_readback;
        ReusableMftOutputSample output_sample;
        DWORD input_alignment{16};
        HRESULT hr = S_OK;
        ID3D11Texture2D* startup_texture{};
        std::shared_ptr<void> startup_retention;
        {
            std::lock_guard lock(mutex);
            startup_texture = std::exchange(startup_nv12_texture, nullptr);
            startup_retention = std::move(startup_nv12_retention);
        }

        if (config.preference != MfH264EncoderPreference::software_only) {
            const bool force_hardware_readback = config.preference
                == MfH264EncoderPreference::hardware_readback_only;
            UINT reset_token{};
            const auto manager_result = MFCreateDXGIDeviceManager(
                &reset_token, &device_manager);
            if (SUCCEEDED(manager_result)
                    && SUCCEEDED(device_manager->ResetDevice(
                        device, reset_token))
                    && activate_hardware_encoder(
                        device_manager,
                        force_hardware_readback,
                        &encoder,
                        &events,
                        &hardware_uses_readback)) {
                if (!hardware_uses_readback
                        || nv12_readback.configure(
                            device, config.width, config.height)) {
                    selected_backend = MfH264EncoderBackend::hardware;
                } else {
                    release(events);
                    release(encoder);
                }
            }
        }

        if (selected_backend == MfH264EncoderBackend::none
                && config.preference
                    != MfH264EncoderPreference::hardware_only
                && config.preference
                    != MfH264EncoderPreference::hardware_readback_only) {
            release(events);
            release(encoder);
            if (activate_software_encoder(&encoder)
                    && nv12_readback.configure(
                        device, config.width, config.height)) {
                selected_backend = MfH264EncoderBackend::software;
            } else {
                release(encoder);
            }
        }

        if (selected_backend == MfH264EncoderBackend::none) hr = E_FAIL;
        MfH264ForceKeyframeSupport detected_keyframe_support{
            MfH264ForceKeyframeSupport::unknown};
        if (SUCCEEDED(hr)) {
            ICodecAPI* candidate{};
            if (SUCCEEDED(encoder->QueryInterface(
                        IID_PPV_ARGS(&candidate)))
                    && candidate->IsSupported(
                        &CODECAPI_AVEncVideoForceKeyFrame) == S_OK) {
                codec_api = candidate;
                detected_keyframe_support =
                    MfH264ForceKeyframeSupport::supported;
            } else {
                release(candidate);
                detected_keyframe_support =
                    MfH264ForceKeyframeSupport::unsupported;
            }
        }
        if (SUCCEEDED(hr)) {
            MFT_INPUT_STREAM_INFO input_info{};
            if (SUCCEEDED(encoder->GetInputStreamInfo(0, &input_info))) {
                input_alignment = std::max<DWORD>(
                    input_alignment, input_info.cbAlignment);
            }
        }
        if (SUCCEEDED(hr)
                && selected_backend == MfH264EncoderBackend::hardware) {
            event_queue = new (std::nothrow) AsyncMftEventQueue(events);
            hr = event_queue ? event_queue->start() : E_OUTOFMEMORY;
        }
        if (SUCCEEDED(hr)
                && selected_backend == MfH264EncoderBackend::hardware) {
            hr = encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        }
        if (SUCCEEDED(hr)) {
            hr = encoder->ProcessMessage(
                MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        }
        if (SUCCEEDED(hr)) {
            hr = encoder->ProcessMessage(
                MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        }

        int requested_inputs{};
        if (SUCCEEDED(hr)
                && selected_backend == MfH264EncoderBackend::hardware) {
            // Activating an asynchronous hardware MFT is not enough to prove
            // that it can encode on the selected adapter. Some Intel drivers
            // accept every setup call but never emit the first NeedInput
            // event, leaving Cast connected to a permanently empty stream.
            const auto deadline = std::chrono::steady_clock::now()
                + kHardwareStartupEventTimeout;
            while (requested_inputs == 0
                    && std::chrono::steady_clock::now() < deadline) {
                if (!event_queue->wait_until(deadline)) break;
                QueuedMftEvent event{};
                while (event_queue->try_pop(event)) {
                    if (FAILED(event.status)) {
                        hr = event.status;
                        break;
                    }
                    if (event.type == METransformNeedInput) {
                        ++requested_inputs;
                    }
                }
                if (FAILED(hr)) {
                    break;
                }
            }
            if (SUCCEEDED(hr) && requested_inputs == 0) {
                hr = MF_E_NOTACCEPTING;
            }
        }

        // Validate the exact production NV12 surface against the exact MFT
        // instance that will encode the stream. This replaces the old second,
        // temporary encoder probe: no vendor encoder is activated twice and a
        // driver rejection immediately advances the host's existing profile
        // loop before the DLNA endpoint is exposed.
        const bool validated_startup_sample = startup_texture != nullptr;
        bool retained_startup_sample_async{};
        if (SUCCEEDED(hr) && startup_texture) {
            std::vector<std::uint8_t> startup_bytes;
            IMFSample* startup_sample{};
            std::shared_ptr<std::atomic_bool> released;
            if (selected_backend == MfH264EncoderBackend::hardware
                    && !hardware_uses_readback) {
                try {
                    released = std::make_shared<std::atomic_bool>(false);
                } catch (...) {
                    hr = E_OUTOFMEMORY;
                }
                if (SUCCEEDED(hr)) {
                    startup_sample = make_dxgi_sample(
                        startup_texture,
                        0,
                        static_cast<LONGLONG>(10000000ULL
                            * config.frame_rate_den
                            / config.frame_rate_num),
                        true,
                        std::move(startup_retention),
                        released);
                }
            } else if (nv12_readback.read(
                           startup_texture, startup_bytes)) {
                startup_sample = make_memory_sample(
                    startup_bytes,
                    0,
                    static_cast<LONGLONG>(10000000ULL
                        * config.frame_rate_den / config.frame_rate_num),
                    true,
                    input_alignment);
            }
            if (SUCCEEDED(hr) && !startup_sample) hr = E_FAIL;
            if (SUCCEEDED(hr)) {
                hr = encoder->ProcessInput(0, startup_sample, 0);
            }
            release(startup_sample);
            retained_startup_sample_async = SUCCEEDED(hr) && released
                && !released->load(std::memory_order_acquire);
            if (SUCCEEDED(hr)
                    && selected_backend == MfH264EncoderBackend::hardware) {
                --requested_inputs;
            }
        }
        release(startup_texture);
        startup_retention.reset();

        {
            std::lock_guard lock(mutex);
            startup_succeeded = SUCCEEDED(hr);
            startup_complete = true;
            startup_retained_async = retained_startup_sample_async;
            running = SUCCEEDED(hr);
            if (SUCCEEDED(hr) && validated_startup_sample) {
                first_input = false;
            }
            active_backend = SUCCEEDED(hr)
                ? selected_backend : MfH264EncoderBackend::none;
            keyframe_support = SUCCEEDED(hr)
                ? detected_keyframe_support
                : MfH264ForceKeyframeSupport::unknown;
        }
        startup_ready.notify_one();

        if (selected_backend == MfH264EncoderBackend::hardware) {
            std::vector<std::uint8_t> nv12_bytes;
            while (SUCCEEDED(hr)) {
                {
                    std::lock_guard lock(mutex);
                    if (stop_requested) break;
                }

                bool did_work = false;
                QueuedMftEvent event{};
                while (event_queue->try_pop(event)) {
                    if (FAILED(event.status)) {
                        hr = event.status;
                        break;
                    }
                    if (event.type == METransformNeedInput) {
                        ++requested_inputs;
                        did_work = true;
                    } else if (event.type == METransformHaveOutput) {
                        auto output_result = consume_encoded_output(
                            encoder,
                            output_sample,
                            [this](const std::uint8_t* data,
                                    std::size_t size,
                                    std::int64_t pts90k,
                                    bool keyframe) {
                                emit_access_unit(
                                    data, size, pts90k, keyframe);
                            });
                        if (output_result == MF_E_TRANSFORM_STREAM_CHANGE) {
                            output_result = renegotiate_h264_output(
                                encoder, config);
                            did_work = SUCCEEDED(output_result);
                            if (SUCCEEDED(output_result)) continue;
                        }
                        if (FAILED(output_result)) {
                            hr = output_result;
                            break;
                        }
                        did_work = true;
                    }
                }

                while (SUCCEEDED(hr) && requested_inputs > 0) {
                    PendingTexture next;
                    {
                        std::lock_guard lock(mutex);
                        if (pending.empty()) break;
                        next = pending.front();
                        pending.pop_front();
                    }
                    IMFSample* sample{};
                    if (hardware_uses_readback) {
                        if (nv12_readback.read(
                                next.texture, nv12_bytes)) {
                            sample = make_memory_sample(
                                nv12_bytes,
                                next.timestamp,
                                next.duration,
                                next.discontinuity,
                                input_alignment);
                        }
                    } else {
                        sample = make_dxgi_sample(
                            next.texture,
                            next.timestamp,
                            next.duration,
                            next.discontinuity,
                            std::move(next.retention));
                    }
                    if (!sample) {
                        hr = E_FAIL;
                        break;
                    }
                    const auto diagnostic_flags =
                        prepare_input_diagnostic_flags(next, codec_api);
                    hr = encoder->ProcessInput(0, sample, 0);
                    release(sample);
                    if (SUCCEEDED(hr)) {
                        if (diagnostics) {
                            const auto correlation_id = next.duration > 0
                                ? static_cast<std::uint64_t>(
                                    next.timestamp / next.duration)
                                : 0;
                            diagnostics->record(
                                CastPipelineEvent::encoder_process_input,
                                correlation_id,
                                static_cast<std::uint64_t>(next.timestamp),
                                diagnostic_flags);
                        }
                        --requested_inputs;
                        did_work = true;
                    }
                }

                if (SUCCEEDED(hr) && !did_work) {
                    static_cast<void>(event_queue->wait_for(
                        std::chrono::milliseconds(2)));
                }
            }
        } else if (selected_backend == MfH264EncoderBackend::software) {
            std::vector<std::uint8_t> nv12_bytes;
            while (SUCCEEDED(hr)) {
                PendingTexture next;
                {
                    std::unique_lock lock(mutex);
                    state_changed.wait(lock, [&] {
                        return stop_requested || !pending.empty();
                    });
                    if (stop_requested) break;
                    next = pending.front();
                    pending.pop_front();
                }

                if (!nv12_readback.read(next.texture, nv12_bytes)) {
                    hr = E_FAIL;
                    break;
                }
                auto* sample = make_memory_sample(
                    nv12_bytes,
                    next.timestamp,
                    next.duration,
                    next.discontinuity,
                    input_alignment);
                if (!sample) {
                    hr = E_FAIL;
                    break;
                }

                auto diagnostic_flags =
                    prepare_input_diagnostic_flags(next, codec_api);
                auto input_result = encoder->ProcessInput(0, sample, 0);
                if (input_result == MF_E_NOTACCEPTING) {
                    hr = drain_software_output(encoder, output_sample);
                    if (SUCCEEDED(hr)) {
                        // The first ProcessInput did not accept this sample;
                        // arm the one-shot control again immediately before
                        // retrying the same input.
                        diagnostic_flags =
                            prepare_input_diagnostic_flags(next, codec_api);
                        input_result = encoder->ProcessInput(0, sample, 0);
                    }
                }
                release(sample);
                if (FAILED(input_result)) {
                    hr = input_result;
                    break;
                }
                if (diagnostics) {
                    const auto correlation_id = next.duration > 0
                        ? static_cast<std::uint64_t>(
                            next.timestamp / next.duration)
                        : 0;
                    diagnostics->record(
                        CastPipelineEvent::encoder_process_input,
                        correlation_id,
                        static_cast<std::uint64_t>(next.timestamp),
                        diagnostic_flags);
                }
                hr = drain_software_output(encoder, output_sample);
            }
        }

        if (event_queue) event_queue->stop();
        if (encoder) {
            static_cast<void>(encoder->ProcessMessage(
                MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0));
            static_cast<void>(encoder->ProcessMessage(
                MFT_MESSAGE_COMMAND_DRAIN, 0));
            if (selected_backend == MfH264EncoderBackend::software) {
                static_cast<void>(drain_software_output(
                    encoder, output_sample));
            }
            static_cast<void>(encoder->ProcessMessage(
                MFT_MESSAGE_NOTIFY_END_STREAMING, 0));
            if (selected_backend == MfH264EncoderBackend::hardware) {
                static_cast<void>(encoder->ProcessMessage(
                    MFT_MESSAGE_SET_D3D_MANAGER, 0));
            }
            IMFShutdown* shutdown{};
            if (SUCCEEDED(encoder->QueryInterface(IID_PPV_ARGS(&shutdown)))) {
                static_cast<void>(shutdown->Shutdown());
            }
            release(shutdown);
        }
        release(event_queue);
        release(events);
        release(codec_api);
        release(encoder);
        release(device_manager);
        {
            std::lock_guard lock(mutex);
            running = false;
            active_backend = MfH264EncoderBackend::none;
        }
        if (SUCCEEDED(com)) CoUninitialize();
    }
};

MfD3D11H264Encoder::MfD3D11H264Encoder()
        : impl_(std::make_unique<Impl>()) {}

MfD3D11H264Encoder::~MfD3D11H264Encoder() = default;

bool MfD3D11H264Encoder::start(
        void* d3d11_device,
        const MfD3D11H264EncoderConfig& config,
        OutputCallback output,
        CastPipelineDiagnostics* diagnostics,
        void* startup_nv12_texture,
        std::shared_ptr<void> startup_retention) {
    return impl_ && impl_->start(
        d3d11_device, config, std::move(output), diagnostics,
        startup_nv12_texture, std::move(startup_retention));
}

void MfD3D11H264Encoder::stop() {
    if (impl_) impl_->stop();
}

bool MfD3D11H264Encoder::is_running() const noexcept {
    return impl_ && impl_->is_running();
}

MfH264EncoderBackend MfD3D11H264Encoder::backend() const noexcept {
    return impl_ ? impl_->backend() : MfH264EncoderBackend::none;
}

MfH264ForceKeyframeSupport
MfD3D11H264Encoder::force_keyframe_support() const noexcept {
    return impl_ ? impl_->force_keyframe_support()
        : MfH264ForceKeyframeSupport::unknown;
}

bool MfD3D11H264Encoder::startup_sample_retained_async() const noexcept {
    return impl_ && impl_->startup_sample_retained_async();
}

bool MfD3D11H264Encoder::prepare_input_texture(
        void* composed_bgra_texture) {
    return impl_ && impl_->prepare_input_texture(composed_bgra_texture);
}

bool MfD3D11H264Encoder::submit_texture(
        void* composed_bgra_texture,
        std::int64_t sample_time_100ns,
        std::int64_t sample_duration_100ns,
        bool force_keyframe) {
    return impl_ && impl_->submit_texture(
        composed_bgra_texture,
        sample_time_100ns,
        sample_duration_100ns,
        force_keyframe);
}

bool MfD3D11H264Encoder::submit_repeated_texture(
        void* unchanged_bgra_texture,
        std::int64_t sample_time_100ns,
        std::int64_t sample_duration_100ns,
        bool force_keyframe) {
    return impl_ && impl_->submit_repeated_texture(
        unchanged_bgra_texture,
        sample_time_100ns,
        sample_duration_100ns,
        force_keyframe);
}

bool MfD3D11H264Encoder::submit_nv12_texture(
        void* nv12_texture,
        std::shared_ptr<void> retention,
        std::int64_t sample_time_100ns,
        std::int64_t sample_duration_100ns,
        bool force_keyframe) {
    return impl_ && impl_->submit_nv12_texture(
        nv12_texture,
        std::move(retention),
        sample_time_100ns,
        sample_duration_100ns,
        force_keyframe);
}
