#pragma once

#include "audio_backend.h"
#include "directsound_backend.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

class AudioPlayer {
public:
    AudioPlayer()
        : backend_(std::make_unique<DirectSoundPcmAudioBackend>()) {}

    ~AudioPlayer() { close(); }

    bool open(const std::wstring& path) {
        if (!backend_->open(path)) return false;
        set_volume(volume_percent_);
        set_local_output_enabled(local_output_enabled_);
        set_key_semitones(key_semitones_);
        return true;
    }

    bool open_both(
            const std::wstring& vocal,
            const std::wstring& accompaniment) {
        if (!vocal.empty()) {
            if (!backend_->open(vocal)) return false;
            if (!accompaniment.empty()) {
                static_cast<void>(
                    backend_->load_second_track(accompaniment));
            }
        } else if (!accompaniment.empty()) {
            if (!backend_->open(accompaniment)) return false;
        } else {
            return false;
        }
        set_volume(volume_percent_);
        set_local_output_enabled(local_output_enabled_);
        set_key_semitones(key_semitones_);
        return true;
    }

    bool load_second_track(const std::wstring& path) {
        return backend_->load_second_track(path);
    }

    bool set_track(int track) {
        if (!backend_->is_open()) return false;
        backend_->set_active_track(track);
        return true;
    }

    void close() { backend_->close(); }

    bool play()  { return backend_->play(); }
    bool pause() { return backend_->pause(); }
    bool stop()  { return backend_->stop(); }

    bool seek(double seconds) {
        return backend_->seek(seconds);
    }

    bool set_volume(int percent) {
        volume_percent_ = std::clamp(percent, 0, 100);
        return backend_->set_volume(volume_percent_);
    }

    bool set_local_output_enabled(bool enabled) {
        local_output_enabled_ = enabled;
        return backend_->set_local_output_enabled(enabled);
    }

    bool set_key_semitones(int semitones) {
        key_semitones_ = std::clamp(semitones, -6, 6);
        return backend_->set_key_semitones(key_semitones_);
    }

    double position() const { return backend_->position(); }
    double duration() const { return backend_->duration(); }
    bool is_open() const noexcept { return backend_->is_open(); }
    bool has_ended() const noexcept { return backend_->has_ended(); }
    ProcessedPcmFormat processed_pcm_format() const {
        return backend_->processed_pcm_format();
    }
    std::size_t processed_pcm_available_frames() const {
        return backend_->processed_pcm_available_frames();
    }
    std::size_t read_processed_pcm(
            std::int16_t* output, std::size_t max_frames) {
        return backend_->read_processed_pcm(output, max_frames);
    }
    std::size_t read_processed_pcm_at(
            std::uint64_t timeline_start_frame,
            std::int16_t* output,
            std::size_t max_frames) {
        return backend_->read_processed_pcm_at(
            timeline_start_frame, output, max_frames);
    }

    void swap(AudioPlayer& other) noexcept {
        using std::swap;
        swap(backend_, other.backend_);
        swap(volume_percent_, other.volume_percent_);
        swap(local_output_enabled_, other.local_output_enabled_);
        swap(key_semitones_, other.key_semitones_);
    }

private:
    std::unique_ptr<IAudioBackend> backend_;
    int volume_percent_{72};
    bool local_output_enabled_{true};
    int key_semitones_{};
};
