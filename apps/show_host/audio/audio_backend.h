#pragma once

#include "processed_pcm_ring.h"

#include <cstddef>
#include <cstdint>
#include <string>

struct DecodedAudioBuffer;

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;
    virtual bool open(const std::wstring& path) = 0;
    virtual bool load_second_track(const std::wstring& path) = 0;
    virtual void set_active_track(int track) = 0;
    virtual void close() = 0;
    virtual bool play() = 0;
    virtual bool pause() = 0;
    virtual bool stop() = 0;
    virtual bool seek(double seconds) = 0;
    virtual bool set_volume(int percent) = 0;
    virtual bool set_local_output_enabled(bool enabled) = 0;
    virtual bool set_key_semitones(int semitones) = 0;
    virtual double position() const = 0;
    virtual double duration() const = 0;
    virtual bool is_open() const noexcept = 0;
    // True only after playback reaches the natural end of the active track.
    // Pause and Cast video-underflow gating must not be reported as EOF.
    virtual bool has_ended() const noexcept { return false; }
    virtual ProcessedPcmFormat processed_pcm_format() const {
        return {};
    }
    virtual std::size_t processed_pcm_available_frames() const {
        return 0;
    }
    virtual std::size_t read_processed_pcm(
            std::int16_t*, std::size_t) {
        return 0;
    }
    virtual std::size_t read_processed_pcm_at(
            std::uint64_t, std::int16_t*, std::size_t) {
        return 0;
    }
};
