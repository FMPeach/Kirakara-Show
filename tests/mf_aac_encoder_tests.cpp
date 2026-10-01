#include "../apps/show_host/cast/aac_adts_packetizer.h"
#include "../apps/show_host/cast/mf_aac_encoder.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <vector>

#include <mfapi.h>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    const auto startup = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    expect(SUCCEEDED(startup), "Media Foundation starts");

    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::vector<std::uint8_t>> frames;
    std::vector<std::int64_t> timestamps;
    MfAacEncoder encoder;
    const auto started = encoder.start(
        MfAacEncoderConfig{},
        [&](const std::uint8_t* data, std::size_t size, std::int64_t pts90k) {
            std::lock_guard lock(mutex);
            frames.emplace_back(data, data + size);
            timestamps.push_back(pts90k);
            ready.notify_all();
        });
    expect(started, "Microsoft AAC encoder starts");

    constexpr std::size_t chunk_frames = 1024;
    std::vector<std::int16_t> silence(chunk_frames * 2U);
    for (std::uint64_t chunk = 0; chunk < 8; ++chunk) {
        expect(encoder.submit_pcm(
            silence.data(), chunk_frames, chunk * chunk_frames),
            "PCM chunk enters bounded AAC queue");
    }

    {
        std::unique_lock lock(mutex);
        ready.wait_for(lock, std::chrono::seconds(5), [&] {
            return frames.size() >= 2;
        });
    }
    encoder.stop();
    MFShutdown();

    expect(frames.size() >= 2, "AAC encoder emits multiple raw frames");
    expect(timestamps.front() == 0, "AAC transport starts at zero");
    expect(timestamps[1] > timestamps[0], "AAC PTS is monotonic");

    std::vector<std::uint8_t> adts;
    expect(AacAdtsPacketizer::append_frame(
        AacAdtsConfig{48000, 2, 2},
        frames.front().data(), frames.front().size(), adts),
        "raw MF AAC frame receives a valid ADTS header");
    expect(adts.size() == frames.front().size() + 7U,
        "ADTS frame size includes its seven-byte header");
    expect(adts[0] == 0xFF && (adts[1] & 0xF6U) == 0xF0U,
        "ADTS sync word is present");

    std::cout << "MF AAC encoder tests passed with "
              << frames.size() << " frames.\n";
    return 0;
}
