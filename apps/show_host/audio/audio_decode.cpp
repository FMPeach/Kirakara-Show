#include "audio_decode.h"
#include "decoded_buffer.h"

#include "../host/host_utils.h"

#include <cmath>
#include <cstring>
#include <vector>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfreadwrite")
#pragma comment(lib, "mfuuid")

std::size_t ring_distance_bytes(
        std::size_t from, std::size_t to, std::size_t ring_size) {
    return to >= from ? to - from : ring_size - from + to;
}

// ── Internal helpers ─────────────────────────────────────────────

namespace {

bool get_audio_type_details(IMFSourceReader* reader,
                            DecodedAudioBuffer& buffer) {
    IMFMediaType* current_type{};
    if (FAILED(reader->GetCurrentMediaType(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM, &current_type))) {
        return false;
    }
    UINT32 channels{};
    UINT32 sample_rate{};
    const auto channels_hr =
        current_type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    const auto sample_rate_hr =
        current_type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sample_rate);
    release_com(current_type);
    if (FAILED(channels_hr) || FAILED(sample_rate_hr)
            || channels == 0 || sample_rate == 0) {
        return false;
    }
    buffer.channels = static_cast<int>(channels);
    buffer.sample_rate = static_cast<int>(sample_rate);
    return true;
}

bool set_float_audio_output_type(IMFSourceReader* reader) {
    IMFMediaType* media_type{};
    auto hr = MFCreateMediaType(&media_type);
    if (SUCCEEDED(hr)) {
        hr = media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    }
    if (SUCCEEDED(hr)) {
        hr = media_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    }
    if (SUCCEEDED(hr)) {
        hr = reader->SetCurrentMediaType(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, media_type);
    }
    release_com(media_type);
    return SUCCEEDED(hr);
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────

DecodedAudioBuffer resample_buffer(
        const DecodedAudioBuffer& src,
        int target_rate,
        int target_channels) {
    DecodedAudioBuffer dst;
    dst.sample_rate = target_rate;
    dst.channels = target_channels;
    if (src.samples.empty() || src.sample_rate <= 0 || src.channels <= 0
            || target_rate <= 0 || target_channels <= 0) {
        return dst;
    }

    const auto ratio = static_cast<double>(target_rate) / src.sample_rate;
    const auto src_frames = src.frame_count();
    const auto dst_frames = static_cast<std::size_t>(
        std::llround(static_cast<double>(src_frames) * ratio));
    dst.samples.resize(dst_frames * static_cast<std::size_t>(target_channels),
        0.0f);

    for (std::size_t frame = 0; frame < dst_frames; ++frame) {
        const auto src_pos = static_cast<double>(frame) / ratio;
        const auto src_idx = static_cast<std::size_t>(src_pos);
        const auto frac = src_pos - static_cast<double>(src_idx);
        for (int channel = 0; channel < target_channels; ++channel) {
            const int src_channel = channel < src.channels ? channel : 0;
            const auto a = src_idx < src_frames
                ? src.samples[src_idx * src.channels + src_channel]
                : 0.0f;
            const auto b = src_idx + 1 < src_frames
                ? src.samples[(src_idx + 1) * src.channels + src_channel]
                : a;
            dst.samples[frame * target_channels + channel] =
                static_cast<float>(a + (b - a) * frac);
        }
    }
    return dst;
}

void downmix_to_stereo(DecodedAudioBuffer& buffer) {
    if (buffer.channels <= 2) return;
    const auto frames = buffer.frame_count();
    std::vector<float> stereo(frames * 2);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        double left{};
        double right{};
        int left_count{};
        int right_count{};
        for (int channel = 0; channel < buffer.channels; ++channel) {
            const auto sample = buffer.samples[
                frame * static_cast<std::size_t>(buffer.channels)
                    + static_cast<std::size_t>(channel)];
            if ((channel % 2) == 0) {
                left += sample;
                ++left_count;
            } else {
                right += sample;
                ++right_count;
            }
        }
        stereo[frame * 2] =
            static_cast<float>(left_count ? left / left_count : 0.0);
        stereo[frame * 2 + 1] =
            static_cast<float>(right_count ? right / right_count : left);
    }
    buffer.channels = 2;
    buffer.samples = std::move(stereo);
}

bool decode_audio_to_float_pcm(const std::wstring& path,
                               DecodedAudioBuffer& buffer) {
    buffer = {};
    IMFSourceReader* reader{};
    auto hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
    if (FAILED(hr) || !reader) {
        log_hresult("MFCreateSourceReaderFromURL(audio) failed", hr);
        release_com(reader);
        return false;
    }

    static_cast<void>(
        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE));
    static_cast<void>(reader->SetStreamSelection(
        MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE));
    if (!set_float_audio_output_type(reader)
            || !get_audio_type_details(reader, buffer)) {
        release_com(reader);
        return false;
    }

    while (true) {
        DWORD stream_index{};
        DWORD flags{};
        LONGLONG timestamp{};
        IMFSample* sample{};
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
            &stream_index, &flags, &timestamp, &sample);
        if (FAILED(hr)) {
            log_hresult("ReadSample(audio) failed", hr);
            release_com(sample);
            release_com(reader);
            return false;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            static_cast<void>(get_audio_type_details(reader, buffer));
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            release_com(sample);
            break;
        }
        if (!sample) continue;

        IMFMediaBuffer* media_buffer{};
        hr = sample->ConvertToContiguousBuffer(&media_buffer);
        if (SUCCEEDED(hr) && media_buffer) {
            BYTE* data{};
            DWORD max_length{};
            DWORD current_length{};
            hr = media_buffer->Lock(&data, &max_length, &current_length);
            if (SUCCEEDED(hr) && data && current_length > 0) {
                static_cast<void>(max_length);
                const auto sample_count =
                    current_length / static_cast<DWORD>(sizeof(float));
                const auto* float_data =
                    reinterpret_cast<const float*>(data);
                buffer.samples.insert(buffer.samples.end(),
                    float_data, float_data + sample_count);
            }
            if (data) media_buffer->Unlock();
        }
        release_com(media_buffer);
        release_com(sample);
    }

    release_com(reader);
    if (buffer.sample_rate <= 0 || buffer.channels <= 0
            || buffer.samples.empty()) {
        return false;
    }
    downmix_to_stereo(buffer);
    const auto whole_frames = buffer.frame_count();
    buffer.samples.resize(
        whole_frames * static_cast<std::size_t>(buffer.channels));
    return whole_frames > 0;
}
