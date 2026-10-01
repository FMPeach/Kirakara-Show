#include "mf_aac_encoder.h"

#include "../host/host_utils.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <objbase.h>

namespace {

constexpr std::size_t kPendingChunkLimit = 32;

// CLSID_AACMFTEncoder from wmcodecdsp.h. Keep the identifier local so this
// translation unit does not depend on a vendor import library for the GUID.
constexpr GUID kClsidAacMftEncoder{
    0x93af0c51,
    0x2275,
    0x45d2,
    {0xa3, 0x5b, 0xf2, 0xba, 0x21, 0xca, 0xed, 0x00},
};

template <typename T>
void release(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

bool valid_config(const MfAacEncoderConfig& config) {
    return (config.sample_rate == 44100 || config.sample_rate == 48000)
        && (config.channels == 1 || config.channels == 2)
        && (config.average_bytes_per_second == 12000
            || config.average_bytes_per_second == 16000
            || config.average_bytes_per_second == 20000
            || config.average_bytes_per_second == 24000);
}

std::int64_t hns_from_frame(
        std::uint64_t frame, std::uint32_t sample_rate) {
    return static_cast<std::int64_t>(
        frame * 10000000ULL / sample_rate);
}

std::int64_t pts90k_from_frame(
        std::uint64_t frame, std::uint32_t sample_rate) {
    return static_cast<std::int64_t>(
        frame * 90000ULL / sample_rate);
}

IMFMediaType* make_pcm_type(const MfAacEncoderConfig& config) {
    IMFMediaType* type{};
    auto hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(
            MF_MT_AUDIO_SAMPLES_PER_SECOND, config.sample_rate);
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, config.channels);
    }
    const auto block_alignment = config.channels * 2U;
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(
            MF_MT_AUDIO_BLOCK_ALIGNMENT, block_alignment);
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(
            MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
            config.sample_rate * block_alignment);
    }
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    }
    if (FAILED(hr)) release(type);
    return type;
}

IMFMediaType* make_aac_type(const MfAacEncoderConfig& config) {
    IMFMediaType* type{};
    auto hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(
            MF_MT_AUDIO_SAMPLES_PER_SECOND, config.sample_rate);
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, config.channels);
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(
            MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
            config.average_bytes_per_second);
    }
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(
            MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    }
    if (FAILED(hr)) release(type);
    return type;
}

IMFSample* make_pcm_sample(
        const std::vector<std::int16_t>& samples,
        std::uint64_t start_frame,
        std::size_t frames,
        const MfAacEncoderConfig& config,
        bool discontinuity) {
    IMFSample* sample{};
    IMFMediaBuffer* buffer{};
    const auto bytes = samples.size() * sizeof(std::int16_t);
    auto hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) {
        hr = MFCreateMemoryBuffer(static_cast<DWORD>(bytes), &buffer);
    }
    BYTE* destination{};
    if (SUCCEEDED(hr)) hr = buffer->Lock(&destination, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        std::copy_n(
            reinterpret_cast<const BYTE*>(samples.data()),
            bytes,
            destination);
        buffer->Unlock();
        destination = nullptr;
        hr = buffer->SetCurrentLength(static_cast<DWORD>(bytes));
    }
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
    if (SUCCEEDED(hr)) {
        hr = sample->SetSampleTime(hns_from_frame(
            start_frame, config.sample_rate));
    }
    if (SUCCEEDED(hr)) {
        hr = sample->SetSampleDuration(hns_from_frame(
            frames, config.sample_rate));
    }
    if (SUCCEEDED(hr) && discontinuity) {
        hr = sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
    }
    if (destination) buffer->Unlock();
    release(buffer);
    if (FAILED(hr)) release(sample);
    return sample;
}

struct PendingPcm {
    std::vector<std::int16_t> samples;
    std::size_t frames{};
    std::uint64_t start_frame{};
};

enum class PullResult {
    output,
    need_input,
    failure,
};

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
            info.cbSize, 64U * 1024U);
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
PullResult pull_output(
        IMFTransform* encoder,
        ReusableMftOutputSample& reusable_sample,
        Consumer&& consumer) {
    MFT_OUTPUT_STREAM_INFO info{};
    auto hr = encoder->GetOutputStreamInfo(0, &info);
    if (FAILED(hr)) return PullResult::failure;

    IMFSample* caller_sample{};
    hr = reusable_sample.prepare(info, &caller_sample);
    if (FAILED(hr)) return PullResult::failure;

    MFT_OUTPUT_DATA_BUFFER output{};
    output.dwStreamID = 0;
    output.pSample = caller_sample;
    DWORD status{};
    hr = encoder->ProcessOutput(0, 1, &output, &status);
    static_cast<void>(status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        release(output.pEvents);
        if (output.pSample != caller_sample) release(output.pSample);
        return PullResult::need_input;
    }
    if (FAILED(hr) || !output.pSample) {
        release(output.pEvents);
        if (output.pSample != caller_sample) release(output.pSample);
        return PullResult::failure;
    }

    IMFMediaBuffer* contiguous{};
    hr = output.pSample->ConvertToContiguousBuffer(&contiguous);
    BYTE* data{};
    DWORD length{};
    if (SUCCEEDED(hr)) hr = contiguous->Lock(&data, nullptr, &length);
    if (SUCCEEDED(hr) && data && length > 0) {
        consumer(data, static_cast<std::size_t>(length));
    }
    if (data) contiguous->Unlock();
    release(contiguous);
    release(output.pEvents);
    if (output.pSample != caller_sample) release(output.pSample);
    return SUCCEEDED(hr) && length > 0
        ? PullResult::output : PullResult::failure;
}

}  // namespace

struct MfAacEncoder::Impl {
    mutable std::mutex mutex;
    std::condition_variable state_changed;
    std::condition_variable startup_ready;
    std::thread worker;
    MfAacEncoderConfig config;
    OutputCallback output;
    std::deque<PendingPcm> pending;
    bool stop_requested{};
    bool startup_complete{};
    bool startup_succeeded{};
    bool running{};

    bool start(
            const MfAacEncoderConfig& requested_config,
            OutputCallback requested_output) {
        if (!valid_config(requested_config) || !requested_output) return false;
        std::unique_lock lock(mutex);
        if (worker.joinable()) return running;
        config = requested_config;
        output = std::move(requested_output);
        pending.clear();
        stop_requested = false;
        startup_complete = false;
        startup_succeeded = false;
        running = false;
        try {
            worker = std::thread([this] { worker_main(); });
        } catch (...) {
            output = {};
            return false;
        }
        startup_ready.wait(lock, [&] { return startup_complete; });
        if (startup_succeeded) return true;
        stop_requested = true;
        lock.unlock();
        state_changed.notify_all();
        worker.join();
        lock.lock();
        output = {};
        return false;
    }

    void stop() {
        {
            std::lock_guard lock(mutex);
            if (!worker.joinable()) {
                pending.clear();
                output = {};
                running = false;
                return;
            }
            stop_requested = true;
        }
        state_changed.notify_all();
        worker.join();
        std::lock_guard lock(mutex);
        pending.clear();
        output = {};
        stop_requested = false;
        startup_complete = false;
        startup_succeeded = false;
        running = false;
    }

    bool is_running() const {
        std::lock_guard lock(mutex);
        return running && !stop_requested;
    }

    bool submit_pcm(
            const std::int16_t* samples,
            std::size_t frames,
            std::uint64_t start_frame) {
        if (!samples || frames == 0) return false;
        PendingPcm chunk;
        chunk.frames = frames;
        chunk.start_frame = start_frame;
        chunk.samples.assign(
            samples,
            samples + frames * static_cast<std::size_t>(config.channels));
        {
            std::lock_guard lock(mutex);
            if (!running || stop_requested
                    || pending.size() >= kPendingChunkLimit) {
                return false;
            }
            pending.push_back(std::move(chunk));
        }
        state_changed.notify_one();
        return true;
    }

    void worker_main() {
        const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMFTransform* encoder{};
        auto hr = CoCreateInstance(
            kClsidAacMftEncoder,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&encoder));
        auto* output_type = make_aac_type(config);
        auto* input_type = make_pcm_type(config);
        if (SUCCEEDED(hr)) {
            hr = output_type
                ? encoder->SetOutputType(0, output_type, 0) : E_OUTOFMEMORY;
        }
        if (SUCCEEDED(hr)) {
            hr = input_type
                ? encoder->SetInputType(0, input_type, 0) : E_OUTOFMEMORY;
        }
        release(input_type);
        release(output_type);
        if (SUCCEEDED(hr)) {
            hr = encoder->ProcessMessage(
                MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        }
        if (SUCCEEDED(hr)) {
            hr = encoder->ProcessMessage(
                MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        }
        {
            std::lock_guard lock(mutex);
            startup_succeeded = SUCCEEDED(hr);
            startup_complete = true;
            running = SUCCEEDED(hr);
        }
        startup_ready.notify_one();

        bool first_input = true;
        std::uint64_t expected_start_frame{};
        std::uint64_t next_output_frame{};
        ReusableMftOutputSample output_sample;
        const auto emit_output = [this, &next_output_frame](
                const std::uint8_t* data, std::size_t size) {
            OutputCallback callback;
            {
                std::lock_guard lock(mutex);
                callback = output;
            }
            if (callback) {
                callback(
                    data,
                    size,
                    pts90k_from_frame(next_output_frame, config.sample_rate));
            }
        };
        while (SUCCEEDED(hr)) {
            PendingPcm next;
            {
                std::unique_lock lock(mutex);
                state_changed.wait(lock, [&] {
                    return stop_requested || !pending.empty();
                });
                if (stop_requested) break;
                next = std::move(pending.front());
                pending.pop_front();
            }

            const bool discontinuity = first_input
                || next.start_frame != expected_start_frame;
            if (discontinuity) next_output_frame = next.start_frame;
            if (discontinuity && !first_input) {
                static_cast<void>(encoder->ProcessMessage(
                    MFT_MESSAGE_COMMAND_FLUSH, 0));
                static_cast<void>(encoder->ProcessMessage(
                    MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
            }
            auto* sample = make_pcm_sample(
                next.samples,
                next.start_frame,
                next.frames,
                config,
                discontinuity);
            if (!sample) {
                hr = E_FAIL;
                break;
            }

            hr = encoder->ProcessInput(0, sample, 0);
            if (hr == MF_E_NOTACCEPTING) {
                const auto pulled = pull_output(
                    encoder, output_sample, emit_output);
                if (pulled == PullResult::output) {
                    next_output_frame += 1024U;
                    hr = encoder->ProcessInput(0, sample, 0);
                }
            }
            release(sample);
            if (FAILED(hr)) break;

            first_input = false;
            expected_start_frame = next.start_frame + next.frames;
            for (;;) {
                const auto pulled = pull_output(
                    encoder, output_sample, emit_output);
                if (pulled == PullResult::need_input) break;
                if (pulled == PullResult::failure) {
                    hr = E_FAIL;
                    break;
                }
                next_output_frame += 1024U;
            }
        }

        if (FAILED(hr)) log_hresult("MF AAC encoder failed", hr);
        if (encoder) {
            static_cast<void>(encoder->ProcessMessage(
                MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0));
            static_cast<void>(encoder->ProcessMessage(
                MFT_MESSAGE_COMMAND_DRAIN, 0));
            static_cast<void>(encoder->ProcessMessage(
                MFT_MESSAGE_NOTIFY_END_STREAMING, 0));
        }
        release(encoder);
        {
            std::lock_guard lock(mutex);
            running = false;
        }
        if (SUCCEEDED(com)) CoUninitialize();
    }
};

MfAacEncoder::MfAacEncoder() : impl_(std::make_unique<Impl>()) {}
MfAacEncoder::~MfAacEncoder() = default;

bool MfAacEncoder::start(
        const MfAacEncoderConfig& config,
        OutputCallback output) {
    return impl_ && impl_->start(config, std::move(output));
}

void MfAacEncoder::stop() {
    if (impl_) impl_->stop();
}

bool MfAacEncoder::is_running() const noexcept {
    return impl_ && impl_->is_running();
}

bool MfAacEncoder::submit_pcm(
        const std::int16_t* interleaved_pcm,
        std::size_t frames,
        std::uint64_t start_frame) {
    return impl_ && impl_->submit_pcm(
        interleaved_pcm, frames, start_frame);
}
