#pragma once

#include "../audio/processed_pcm_ring.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// Stateful live resampler for the Cast audio program.  The Stage transport is
// fixed at 48 kHz stereo even when songs switch between 44.1/48 kHz sources.
// It intentionally keeps only the small amount of PCM needed by the next
// transport frame; an underflow becomes silence instead of delayed audio.
class CastPcmResampler {
public:
    static constexpr int kOutputSampleRate = 48000;
    static constexpr int kOutputChannels = 2;

    void reset();
    void prepare_format(ProcessedPcmFormat format);

    [[nodiscard]] std::size_t source_frames_needed(
        std::size_t output_frames) const;

    void append(
        const std::int16_t* samples,
        std::size_t frames,
        ProcessedPcmFormat format);

    // Always writes exactly output_frames of interleaved stereo PCM. Missing
    // live source frames are zero-filled and are not replayed later.
    void render(std::int16_t* output, std::size_t output_frames);

private:
    [[nodiscard]] std::size_t buffered_frames() const noexcept;
    void discard_source_frames(std::size_t frames);

    ProcessedPcmFormat format_;
    std::vector<std::int16_t> stereo_source_;
    double source_position_{};
};
