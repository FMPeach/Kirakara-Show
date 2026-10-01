#include "cast_pcm_resampler.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

std::int16_t clamp_pcm(double sample) {
    const auto rounded = std::llround(sample);
    return static_cast<std::int16_t>(std::clamp<long long>(
        rounded,
        std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max()));
}

}  // namespace

void CastPcmResampler::reset() {
    format_ = {};
    stereo_source_.clear();
    source_position_ = 0.0;
}

void CastPcmResampler::prepare_format(ProcessedPcmFormat format) {
    if (!format.valid()) {
        reset();
        return;
    }
    if (format_.sample_rate == format.sample_rate
            && format_.channels == format.channels) {
        return;
    }
    format_ = format;
    stereo_source_.clear();
    source_position_ = 0.0;
}

std::size_t CastPcmResampler::source_frames_needed(
        std::size_t output_frames) const {
    if (!format_.valid() || output_frames == 0) return 0;
    const auto step = static_cast<double>(format_.sample_rate)
        / static_cast<double>(kOutputSampleRate);
    const auto last_position = source_position_
        + static_cast<double>(output_frames - 1U) * step;
    const auto required = static_cast<std::size_t>(
        std::floor(last_position)) + 2U;
    const auto buffered = buffered_frames();
    return required > buffered ? required - buffered : 0U;
}

void CastPcmResampler::append(
        const std::int16_t* samples,
        std::size_t frames,
        ProcessedPcmFormat format) {
    if (!samples || frames == 0 || !format.valid()) return;
    prepare_format(format);
    if (!format_.valid()) return;

    stereo_source_.reserve(stereo_source_.size() + frames * 2U);
    const auto channels = static_cast<std::size_t>(format_.channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto* source = samples + frame * channels;
        if (channels == 1U) {
            stereo_source_.push_back(source[0]);
            stereo_source_.push_back(source[0]);
            continue;
        }
        if (channels == 2U) {
            stereo_source_.push_back(source[0]);
            stereo_source_.push_back(source[1]);
            continue;
        }

        long long left{};
        long long right{};
        std::size_t left_count{};
        std::size_t right_count{};
        for (std::size_t channel = 0; channel < channels; ++channel) {
            if ((channel & 1U) == 0U) {
                left += source[channel];
                ++left_count;
            } else {
                right += source[channel];
                ++right_count;
            }
        }
        const auto left_sample = left_count
            ? left / static_cast<long long>(left_count) : 0LL;
        const auto right_sample = right_count
            ? right / static_cast<long long>(right_count) : left_sample;
        stereo_source_.push_back(static_cast<std::int16_t>(left_sample));
        stereo_source_.push_back(static_cast<std::int16_t>(right_sample));
    }
}

void CastPcmResampler::render(
        std::int16_t* output, std::size_t output_frames) {
    if (!output || output_frames == 0) return;
    std::fill_n(output, output_frames * 2U, std::int16_t{});
    if (!format_.valid() || stereo_source_.empty()) return;

    const auto step = static_cast<double>(format_.sample_rate)
        / static_cast<double>(kOutputSampleRate);
    const auto available = buffered_frames();
    std::size_t rendered{};
    while (rendered < output_frames) {
        const auto source_index = static_cast<std::size_t>(source_position_);
        if (source_index >= available) break;
        const auto next_index = std::min(source_index + 1U, available - 1U);
        const auto fraction = source_position_
            - static_cast<double>(source_index);
        for (std::size_t channel = 0; channel < 2U; ++channel) {
            const auto a = stereo_source_[source_index * 2U + channel];
            const auto b = stereo_source_[next_index * 2U + channel];
            output[rendered * 2U + channel] = clamp_pcm(
                static_cast<double>(a)
                    + (static_cast<double>(b) - static_cast<double>(a))
                        * fraction);
        }
        ++rendered;
        source_position_ += step;
    }

    const auto consumed = std::min(
        static_cast<std::size_t>(source_position_), buffered_frames());
    discard_source_frames(consumed);
    source_position_ -= static_cast<double>(consumed);

    // Keep a short incomplete tail. Track/key fades are already represented
    // in the final PCM stream and must not be discarded by the Cast tap.
}

std::size_t CastPcmResampler::buffered_frames() const noexcept {
    return stereo_source_.size() / 2U;
}

void CastPcmResampler::discard_source_frames(std::size_t frames) {
    const auto samples = std::min(frames * 2U, stereo_source_.size());
    if (samples == 0) return;
    stereo_source_.erase(
        stereo_source_.begin(),
        stereo_source_.begin() + static_cast<std::ptrdiff_t>(samples));
}
