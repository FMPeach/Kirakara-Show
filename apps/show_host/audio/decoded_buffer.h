#pragma once

#include <cstddef>
#include <vector>

struct DecodedAudioBuffer {
    int sample_rate{};
    int channels{};
    std::vector<float> samples;

    std::size_t frame_count() const noexcept {
        return channels > 0 ? samples.size() / static_cast<std::size_t>(channels)
                            : 0;
    }

    double duration() const noexcept {
        return sample_rate > 0
            ? static_cast<double>(frame_count()) / sample_rate
            : 0.0;
    }
};
