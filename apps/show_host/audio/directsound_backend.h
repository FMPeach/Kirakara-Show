#pragma once

#include "audio_backend.h"
#include "decoded_buffer.h"
#include "audio_decode.h"
#include "../host/host_utils.h"
#include "../show_host_api.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <dsound.h>
#include <windows.h>

#include "signalsmith-stretch.h"

#pragma comment(lib, "dsound")
#pragma comment(lib, "winmm")

class DirectSoundPcmAudioBackend final : public IAudioBackend {
public:
    ~DirectSoundPcmAudioBackend();

    bool open(const std::wstring& path);

    bool load_second_track(const std::wstring& path);

    void set_active_track(int track);

    void close();

    bool play();

    bool pause();

    bool stop();

    bool seek(double seconds);

    bool set_volume(int percent);

    bool set_local_output_enabled(bool enabled) override;

    bool set_key_semitones(int semitones);

    double position() const;

    double duration() const;

    bool is_open() const noexcept;

    bool has_ended() const noexcept override;

    ProcessedPcmFormat processed_pcm_format() const override;

    std::size_t processed_pcm_available_frames() const override;

    std::size_t read_processed_pcm(
        std::int16_t* output, std::size_t max_frames) override;

    std::size_t read_processed_pcm_at(
        std::uint64_t timeline_start_frame,
        std::int16_t* output,
        std::size_t max_frames) override;

private:
    bool create_directsound_buffer();

    void clear_directsound_buffer();

    void reset_stream_state(std::uint64_t frame);

    void reset_stretch_locked();

    std::uint64_t next_output_frame_locked() const;

    std::uint64_t stretch_lookahead_frames_locked();

    void align_read_frame_to_next_output_locked();

    void retarget_buffer_after_track_switch_locked(std::uint64_t play_frame);

    std::uint64_t current_play_frame_locked() const;

    void render_loop();

    void fill_output_buffer();

    void configure_stretch_locked();

    int configure_stretch_for_key_locked( signalsmith::stretch::SignalsmithStretch<float>& stretcher, int key);

    void fill_planar_source_locked( std::uint64_t start_frame, std::size_t frames, std::vector<std::vector<float>>& planar, std::vector<float*>& ptrs);

    void prime_stretch_for_output_frame_locked( std::uint64_t output_frame, std::uint64_t lookahead);

    void render_pcm_frames(std::int16_t* output, std::size_t frames,
        std::uint64_t& timeline_start_frame);

    void render_plain_float_locked(float* output, std::size_t frames);

    void render_plain_float_from_cursor_locked( float* output, std::size_t frames, std::uint64_t& read_frame);

    void render_shifted_float_locked(float* output, std::size_t frames);

    void render_shifted_float_from_cursor_locked( signalsmith::stretch::SignalsmithStretch<float>& stretcher, float* output, std::size_t frames, std::uint64_t& read_frame, std::vector<std::vector<float>>& input, std::vector<std::vector<float>>& stretched_output, std::vector<float*>& input_ptrs, std::vector<float*>& output_ptrs);

    void begin_key_fade_locked( int from_key, int to_key, std::uint64_t output_frame);

    void cancel_key_fade_locked();

    std::uint64_t key_fade_frame_count_locked() const;

    void prime_stretch_instance_for_output_frame_locked( signalsmith::stretch::SignalsmithStretch<float>& stretcher, std::uint64_t output_frame, std::uint64_t lookahead, std::vector<std::vector<float>>& seek_input, std::vector<float*>& seek_input_ptrs);

    void render_key_fade_source_locked( float* output, std::size_t frames);

    void blend_key_fade_locked( float* processed, const float* fade_from, std::size_t fade_frames);

    DecodedAudioBuffer vocal_pcm_;
    DecodedAudioBuffer inst_pcm_;
    DecodedAudioBuffer* active_pcm_{&vocal_pcm_};
    ProcessedPcmRing processed_pcm_ring_;
    std::atomic<std::uint64_t> track_generation_{0};
    IDirectSound8* direct_sound_{};
    IDirectSoundBuffer* buffer_{};
    std::thread worker_;
    mutable std::mutex mutex_;
    // Last position computed under mutex_. position() is polled from the
    // render thread, which must never block: the audio worker holds mutex_
    // across the pitch-shift DSP in render_pcm_frames(), so a plain lock here
    // stalled the render loop for 5-27 ms. See position().
    mutable std::atomic<double> cached_position_{0.0};
    std::condition_variable cv_;
    bool opened_{};
    bool playing_{};
    bool buffer_playing_{};
    std::atomic<bool> ended_{};
    bool stop_thread_{};
    int volume_percent_{72};
    bool local_output_enabled_{true};
    int key_semitones_{};
    std::uint64_t stream_start_frame_{};
    std::uint64_t read_frame_{};
    std::uint64_t submitted_frames_{};
    std::size_t next_write_byte_{};
    std::size_t buffer_bytes_{};
    WORD block_align_{};
    signalsmith::stretch::SignalsmithStretch<float> stretch_;
    bool stretch_configured_{};
    int stretch_key_semitones_{};
    int stretch_latency_frames_{};
    std::vector<std::vector<float>> stretch_seek_input_;
    std::vector<float*> stretch_seek_input_ptrs_;
    std::vector<std::vector<float>> stretch_input_;
    std::vector<std::vector<float>> stretch_output_;
    std::vector<float*> stretch_input_ptrs_;
    std::vector<float*> stretch_output_ptrs_;
    signalsmith::stretch::SignalsmithStretch<float> transition_stretch_;
    bool key_fade_active_{};
    int key_fade_from_key_{};
    std::uint64_t key_fade_total_frames_{};
    std::uint64_t key_fade_done_frames_{};
    std::uint64_t key_fade_plain_read_frame_{};
    std::uint64_t key_fade_shifted_read_frame_{};
    std::vector<std::vector<float>> key_fade_seek_input_;
    std::vector<float*> key_fade_seek_input_ptrs_;
    std::vector<std::vector<float>> key_fade_input_;
    std::vector<std::vector<float>> key_fade_output_;
    std::vector<float*> key_fade_input_ptrs_;
    std::vector<float*> key_fade_output_ptrs_;
};
