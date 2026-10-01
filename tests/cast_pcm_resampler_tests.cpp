#include "../apps/show_host/cast/cast_pcm_resampler.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void test_48k_stereo_passthrough() {
    CastPcmResampler resampler;
    constexpr std::size_t output_frames = 800;
    const ProcessedPcmFormat format{48000, 2};
    const auto needed = resampler.source_frames_needed(output_frames);
    expect(needed == 0, "unconfigured resampler requests no source");
    resampler.prepare_format(format);
    const auto configured_needed = resampler.source_frames_needed(output_frames);
    expect(configured_needed == output_frames + 1,
        "48 kHz interpolation keeps one look-ahead frame");

    std::vector<std::int16_t> input(configured_needed * 2U);
    for (std::size_t frame = 0; frame < configured_needed; ++frame) {
        input[frame * 2U] = static_cast<std::int16_t>(frame);
        input[frame * 2U + 1U] = static_cast<std::int16_t>(-static_cast<int>(frame));
    }
    resampler.append(input.data(), configured_needed, format);
    std::vector<std::int16_t> output(output_frames * 2U);
    resampler.render(output.data(), output_frames);
    for (std::size_t frame = 0; frame < output_frames; ++frame) {
        expect(output[frame * 2U] == input[frame * 2U],
            "48 kHz left channel remains bit exact");
        expect(output[frame * 2U + 1U] == input[frame * 2U + 1U],
            "48 kHz right channel remains bit exact");
    }
}

void test_44k1_constant_resample() {
    CastPcmResampler resampler;
    constexpr std::size_t output_frames = 1600;
    const ProcessedPcmFormat format{44100, 2};
    resampler.prepare_format(format);
    const auto needed = resampler.source_frames_needed(output_frames);
    expect(needed > 1400 && needed < 1500,
        "44.1 kHz source requirement is in the expected range");
    std::vector<std::int16_t> input(needed * 2U, 1234);
    resampler.append(input.data(), needed, format);
    std::vector<std::int16_t> output(output_frames * 2U);
    resampler.render(output.data(), output_frames);
    expect(std::all_of(output.begin(), output.end(), [](std::int16_t value) {
        return value == 1234;
    }), "constant 44.1 kHz signal stays constant at 48 kHz");
}

void test_short_tail_survives_chunk_boundary() {
    CastPcmResampler resampler;
    const ProcessedPcmFormat format{48000, 1};
    resampler.prepare_format(format);
    const std::int16_t source[]{2000, 2000, 2000};
    resampler.append(source, 3, format);
    std::vector<std::int16_t> output(4U * 2U, -1);
    resampler.render(output.data(), 1);

    const std::int16_t continuation[]{3000, 3000, 3000};
    resampler.append(continuation, 3, format);
    std::fill(output.begin(), output.end(), -1);
    resampler.render(output.data(), 4);
    expect(output[0] == 2000 && output[1] == 2000,
        "unconsumed final PCM survives across source chunks");
    expect(output[4] == 3000 && output[5] == 3000,
        "new PCM follows the preserved short tail");
}

void test_underflow_is_silence() {
    CastPcmResampler resampler;
    const ProcessedPcmFormat format{48000, 1};
    resampler.prepare_format(format);
    const std::int16_t source[]{2000};
    resampler.append(source, 1, format);
    std::vector<std::int16_t> output(8U * 2U, -1);
    resampler.render(output.data(), 8);
    expect(output[0] == 2000 && output[1] == 2000,
        "mono source is duplicated to stereo");
    expect(output[2] == 0 && output[3] == 0,
        "missing live source becomes silence");
}

}  // namespace

int main() {
    test_48k_stereo_passthrough();
    test_44k1_constant_resample();
    test_short_tail_survives_chunk_boundary();
    test_underflow_is_silence();
    std::cout << "Cast PCM resampler tests passed.\n";
    return 0;
}
