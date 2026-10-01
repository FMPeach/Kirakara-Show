#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct AacAdtsConfig {
    int sample_rate{};
    int channel_count{};
    // MPEG-4 audio object type. 2 = AAC LC.
    int audio_object_type{2};
};

class AacAdtsPacketizer {
public:
    static constexpr std::size_t kHeaderSize = 7;

    [[nodiscard]] static int sample_rate_index(int sample_rate);

    [[nodiscard]] static bool append_frame(
        const AacAdtsConfig& config,
        const std::uint8_t* raw_aac,
        std::size_t raw_size,
        std::vector<std::uint8_t>& out);
};
