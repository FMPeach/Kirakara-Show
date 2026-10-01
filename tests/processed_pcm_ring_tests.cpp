#include "../apps/show_host/audio/processed_pcm_ring.h"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    ProcessedPcmRing ring;
    ring.configure(10, 2, 0.5);  // five stereo frames

    const auto format = ring.format();
    expect(format.sample_rate == 10, "sample rate should be retained");
    expect(format.channels == 2, "channel count should be retained");

    std::vector<std::int16_t> first{
        1, 2,
        3, 4,
        5, 6,
    };
    ring.push(first.data(), 3);
    expect(ring.available_frames() == 3, "three frames should be readable");

    std::vector<std::int16_t> out(4);
    expect(ring.read(out.data(), 2) == 2, "read should return two frames");
    expect(out[0] == 1 && out[1] == 2 && out[2] == 3 && out[3] == 4,
        "read should preserve FIFO order");
    expect(ring.available_frames() == 1, "one frame should remain");

    std::vector<std::int16_t> second{
        7, 8,
        9, 10,
        11, 12,
        13, 14,
        15, 16,
        17, 18,
    };
    ring.push(second.data(), 6);
    expect(ring.available_frames() == 5,
        "overflow should keep only ring capacity");

    std::vector<std::int16_t> overflow(10);
    expect(ring.read(overflow.data(), 5) == 5,
        "read should return remaining capacity");
    expect(overflow[0] == 9 && overflow[1] == 10,
        "overflow should drop the oldest frame");
    expect(overflow[8] == 17 && overflow[9] == 18,
        "overflow should keep the newest frame");

    ring.push(first.data(), 3);
    ring.clear();
    expect(ring.read(out.data(), 2) == 0, "clear should remove readable data");

    ring.configure(10, 2, 0.5);
    ring.push_timed(first.data(), 3, 100);
    std::vector<std::int16_t> timed(10, -1);
    expect(ring.read_at(101, timed.data(), 2) == 2,
        "timeline lookup should read requested frames");
    expect(timed[0] == 3 && timed[1] == 4
           && timed[2] == 5 && timed[3] == 6,
        "timeline lookup should preserve sample positions");
    std::fill(timed.begin(), timed.end(), -1);
    expect(ring.read_at(101, timed.data(), 2) == 2,
        "timeline lookup should be non-destructive");

    const std::vector<std::int16_t> replacement{
        21, 22,
        23, 24,
        25, 26,
    };
    ring.push_timed(replacement.data(), 3, 102);
    std::fill(timed.begin(), timed.end(), -1);
    expect(ring.read_at(100, timed.data(), 5) == 5,
        "overlap should retain prefix and replace future tail");
    expect(timed[0] == 1 && timed[1] == 2
           && timed[2] == 3 && timed[3] == 4
           && timed[4] == 21 && timed[5] == 22,
        "replacement should begin at its timeline frame");

    ring.push_timed(second.data(), 6, 105);
    std::vector<std::int16_t> latest(10, -1);
    expect(ring.read_at(106, latest.data(), 5) == 5,
        "overflow should expose the retained timeline range");
    expect(ring.read_at(105, latest.data(), 1) == 0,
        "overflow should reject a dropped timeline frame");

    std::cout << "ProcessedPcmRing test passed.\n";
    return 0;
}
