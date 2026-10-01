#pragma once

#include "audio_backend.h"
#include "decoded_buffer.h"

#include <dsound.h>
#include <dshow.h>
#include <windows.h>

// ── DirectShow-based audio backend (fallback, no pitch shift) ────

class DirectShowAudioBackend final : public IAudioBackend {
public:
    ~DirectShowAudioBackend() override;
    bool open(const std::wstring& path) override;
    bool load_second_track(const std::wstring&) override;
    void set_active_track(int) override;
    void close() override;
    bool play() override;
    bool pause() override;
    bool stop() override;
    bool seek(double seconds) override;
    bool set_volume(int percent) override;
    bool set_local_output_enabled(bool enabled) override;
    bool set_key_semitones(int semitones) override;
    double position() const override;
    double duration() const override;
    bool is_open() const noexcept override;

private:
    bool apply_output_volume();

    IGraphBuilder* graph_{};
    IMediaControl* control_{};
    IMediaSeeking* seeking_{};
    IBasicAudio* basic_audio_{};
    bool opened_{};
    bool playing_{};
    int volume_percent_{72};
    bool local_output_enabled_{true};
    int key_semitones_{};
};
