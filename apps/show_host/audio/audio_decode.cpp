#include "audio_decode.h"
#include "decoded_buffer.h"
#include "../media/ffmpeg_decode.h"

#include <cmath>
#include <cstring>
#include <vector>

std::size_t ring_distance_bytes(
        std::size_t from, std::size_t to, std::size_t ring_size) {
    return to >= from ? to - from : ring_size - from + to;
}

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
    return ffmpeg_decode_audio(path, buffer);
}