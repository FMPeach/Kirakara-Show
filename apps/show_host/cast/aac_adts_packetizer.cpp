#include "aac_adts_packetizer.h"

#include <algorithm>
#include <array>

int AacAdtsPacketizer::sample_rate_index(int sample_rate) {
    constexpr std::array<int, 13> rates{
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350,
    };
    const auto found = std::find(rates.begin(), rates.end(), sample_rate);
    if (found == rates.end()) return -1;
    return static_cast<int>(found - rates.begin());
}

bool AacAdtsPacketizer::append_frame(
        const AacAdtsConfig& config,
        const std::uint8_t* raw_aac,
        std::size_t raw_size,
        std::vector<std::uint8_t>& out) {
    if (!raw_aac || raw_size == 0) return false;
    const auto sr_index = sample_rate_index(config.sample_rate);
    if (sr_index < 0) return false;
    if (config.channel_count <= 0 || config.channel_count > 7) return false;
    if (config.audio_object_type < 1 || config.audio_object_type > 4) {
        return false;
    }
    const auto frame_length = raw_size + kHeaderSize;
    if (frame_length > 0x1FFFU) return false;

    const auto profile = static_cast<std::uint8_t>(
        config.audio_object_type - 1);
    const auto channel_config = static_cast<std::uint8_t>(
        config.channel_count);

    const auto start = out.size();
    out.resize(start + frame_length);
    auto* header = out.data() + start;
    header[0] = 0xFF;
    header[1] = 0xF1;  // sync, MPEG-4, no CRC
    header[2] = static_cast<std::uint8_t>(
        (profile << 6U)
        | (static_cast<std::uint8_t>(sr_index) << 2U)
        | ((channel_config >> 2U) & 0x01U));
    header[3] = static_cast<std::uint8_t>(
        ((channel_config & 0x03U) << 6U)
        | ((frame_length >> 11U) & 0x03U));
    header[4] = static_cast<std::uint8_t>((frame_length >> 3U) & 0xFFU);
    header[5] = static_cast<std::uint8_t>(
        ((frame_length & 0x07U) << 5U) | 0x1FU);
    header[6] = 0xFC;

    std::copy_n(raw_aac, raw_size, out.data() + start + kHeaderSize);
    return true;
}
