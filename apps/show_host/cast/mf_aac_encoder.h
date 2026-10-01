#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

struct MfAacEncoderConfig {
    std::uint32_t sample_rate{48000};
    std::uint32_t channels{2};
    // Microsoft AAC MFT expresses its supported bit rates in bytes/second.
    std::uint32_t average_bytes_per_second{20000};  // 160 kbit/s
};

// Asynchronous wrapper around the Windows Media Foundation AAC-LC encoder.
// Input is the final interleaved 16-bit PCM produced by the App audio chain;
// output is one raw AAC frame per callback (no ADTS header). The callback data
// is borrowed and remains valid only until the callback returns.
class MfAacEncoder {
public:
    using OutputCallback = std::function<void(
        const std::uint8_t* data,
        std::size_t size,
        std::int64_t pts90k)>;

    MfAacEncoder();
    ~MfAacEncoder();

    MfAacEncoder(const MfAacEncoder&) = delete;
    MfAacEncoder& operator=(const MfAacEncoder&) = delete;

    [[nodiscard]] bool start(
        const MfAacEncoderConfig& config,
        OutputCallback output);
    void stop();

    [[nodiscard]] bool is_running() const noexcept;

    // start_frame is measured on the continuous Cast transport clock at the
    // configured sample rate. The queue is bounded; false means this chunk
    // was dropped instead of blocking Stage rendering.
    [[nodiscard]] bool submit_pcm(
        const std::int16_t* interleaved_pcm,
        std::size_t frames,
        std::uint64_t start_frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
