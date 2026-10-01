#include "../apps/show_host/cast/aac_adts_packetizer.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::size_t adts_frame_length(const std::vector<std::uint8_t>& frame) {
    return (static_cast<std::size_t>(frame[3] & 0x03U) << 11U)
        | (static_cast<std::size_t>(frame[4]) << 3U)
        | ((frame[5] >> 5U) & 0x07U);
}

void test_sample_rate_index() {
    expect(AacAdtsPacketizer::sample_rate_index(96000) == 0,
        "96000 Hz index mismatch");
    expect(AacAdtsPacketizer::sample_rate_index(44100) == 4,
        "44100 Hz index mismatch");
    expect(AacAdtsPacketizer::sample_rate_index(48000) == 3,
        "48000 Hz index mismatch");
    expect(AacAdtsPacketizer::sample_rate_index(12345) < 0,
        "unsupported rate should be rejected");
}

void test_adts_header() {
    const std::vector<std::uint8_t> raw{0x11, 0x22, 0x33, 0x44};
    std::vector<std::uint8_t> out;
    const AacAdtsConfig config{
        .sample_rate = 44100,
        .channel_count = 2,
        .audio_object_type = 2,
    };
    expect(AacAdtsPacketizer::append_frame(
        config, raw.data(), raw.size(), out), "ADTS append failed");
    expect(out.size() == raw.size() + AacAdtsPacketizer::kHeaderSize,
        "ADTS frame size mismatch");
    expect(out[0] == 0xFF && out[1] == 0xF1, "ADTS sync mismatch");
    expect((out[2] >> 6U) == 1, "AAC-LC profile mismatch");
    expect(((out[2] >> 2U) & 0x0FU) == 4, "sample-rate index mismatch");
    const auto channels = static_cast<int>(((out[2] & 0x01U) << 2U)
        | ((out[3] >> 6U) & 0x03U));
    expect(channels == 2, "channel config mismatch");
    expect(adts_frame_length(out) == out.size(), "ADTS length mismatch");
    expect(out[7] == raw[0] && out[10] == raw[3],
        "ADTS payload should follow header");
}

void test_rejects_invalid_config() {
    const std::vector<std::uint8_t> raw{0x55};
    std::vector<std::uint8_t> out;
    expect(!AacAdtsPacketizer::append_frame(
        AacAdtsConfig{.sample_rate = 12345, .channel_count = 2},
        raw.data(), raw.size(), out), "invalid rate should fail");
    expect(!AacAdtsPacketizer::append_frame(
        AacAdtsConfig{.sample_rate = 44100, .channel_count = 0},
        raw.data(), raw.size(), out), "invalid channels should fail");
    expect(!AacAdtsPacketizer::append_frame(
        AacAdtsConfig{.sample_rate = 44100, .channel_count = 2},
        nullptr, raw.size(), out), "null payload should fail");
}

}  // namespace

int main() {
    test_sample_rate_index();
    test_adts_header();
    test_rejects_invalid_config();
    std::cout << "AacAdtsPacketizer test passed.\n";
    return 0;
}
