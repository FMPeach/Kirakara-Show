#include "directsound_backend.h"

DirectSoundPcmAudioBackend::~DirectSoundPcmAudioBackend()
{ close(); }

bool DirectSoundPcmAudioBackend::open(const std::wstring& path)
{
        close();
        DecodedAudioBuffer decoded;
        if (!decode_audio_to_float_pcm(path, decoded)) return false;
        vocal_pcm_ = std::move(decoded);
        inst_pcm_ = {};
        active_pcm_ = &vocal_pcm_;
        volume_percent_ = std::clamp(volume_percent_, 0, 100);
        key_semitones_ = std::clamp(key_semitones_, -6, 6);
        if (!create_directsound_buffer()) {
            close();
            return false;
        }
        processed_pcm_ring_.configure(
            vocal_pcm_.sample_rate, vocal_pcm_.channels);
        ended_.store(false, std::memory_order_relaxed);
        opened_ = true;
        reset_stream_state(0);
        stop_thread_ = false;
        worker_ = std::thread([this] { render_loop(); });
        return true;
    }

bool DirectSoundPcmAudioBackend::load_second_track(const std::wstring& path)
{
        if (path.empty()) return false;
        DecodedAudioBuffer decoded;
        if (!decode_audio_to_float_pcm(path, decoded)) return false;
        std::lock_guard lock(mutex_);
        if (decoded.sample_rate != vocal_pcm_.sample_rate
                || decoded.channels != vocal_pcm_.channels) {
            decoded = resample_buffer(decoded, vocal_pcm_.sample_rate,
                vocal_pcm_.channels);
        }
        if (decoded.frame_count() == 0) return false;
        inst_pcm_ = std::move(decoded);
        return true;
    }

void DirectSoundPcmAudioBackend::set_active_track(int track)
{
        std::lock_guard lock(mutex_);
        const auto play_frame = current_play_frame_locked();
        auto* previous_pcm = active_pcm_;
        if (track == SHOW_AUDIO_TRACK_ACCOMPANIMENT
                && inst_pcm_.frame_count() > 0) {
            active_pcm_ = &inst_pcm_;
        } else {
            active_pcm_ = &vocal_pcm_;
        }
        if (active_pcm_ == previous_pcm) return;
        ++track_generation_;
        retarget_buffer_after_track_switch_locked(play_frame);
        ended_.store(
            play_frame >= active_pcm_->frame_count(),
            std::memory_order_relaxed);
        cv_.notify_all();
    }

void DirectSoundPcmAudioBackend::close()
{
        {
            std::lock_guard lock(mutex_);
            stop_thread_ = true;
            playing_ = false;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (buffer_) {
            static_cast<void>(buffer_->Stop());
        }
        release_com(buffer_);
        release_com(direct_sound_);
        vocal_pcm_ = {};
        inst_pcm_ = {};
        active_pcm_ = &vocal_pcm_;
        opened_ = false;
        buffer_playing_ = false;
        stop_thread_ = false;
        ended_.store(false, std::memory_order_relaxed);
        processed_pcm_ring_.reset();
    }

bool DirectSoundPcmAudioBackend::play()
{
        std::lock_guard lock(mutex_);
        if (!opened_) return false;
        playing_ = true;
        cv_.notify_all();
        return true;
    }

bool DirectSoundPcmAudioBackend::pause()
{
        std::lock_guard lock(mutex_);
        if (!opened_) return false;
        playing_ = false;
        if (buffer_) {
            static_cast<void>(buffer_->Stop());
        }
        buffer_playing_ = false;
        processed_pcm_ring_.clear();
        return true;
    }

bool DirectSoundPcmAudioBackend::stop()
{
        std::lock_guard lock(mutex_);
        if (!opened_) return false;
        playing_ = false;
        reset_stream_state(0);
        ended_.store(false, std::memory_order_relaxed);
        return true;
    }

bool DirectSoundPcmAudioBackend::seek(double seconds)
{
        std::lock_guard lock(mutex_);
        if (!opened_) return false;
        if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
        const auto frame = static_cast<std::uint64_t>(
            std::min(seconds, duration()) * active_pcm_->sample_rate);
        playing_ = false;
        reset_stream_state(frame);
        ended_.store(
            frame >= active_pcm_->frame_count(),
            std::memory_order_relaxed);
        cv_.notify_all();
        return true;
    }

bool DirectSoundPcmAudioBackend::set_volume(int percent)
{
        std::lock_guard lock(mutex_);
        volume_percent_ = std::clamp(percent, 0, 100);
        return true;
    }

bool DirectSoundPcmAudioBackend::set_local_output_enabled(bool enabled)
{
        std::lock_guard lock(mutex_);
        local_output_enabled_ = enabled;
        if (!buffer_) return true;
        // This gates only the local device branch; processed PCM stays intact.
        return SUCCEEDED(buffer_->SetVolume(
            enabled ? DSBVOLUME_MAX : DSBVOLUME_MIN));
    }

bool DirectSoundPcmAudioBackend::set_key_semitones(int semitones)
{
        std::lock_guard lock(mutex_);
        const auto key = std::clamp(semitones, -6, 6);
        if (key == key_semitones_) return true;
        const auto previous_key = key_semitones_;
        const auto output_frame = next_output_frame_locked();
        key_semitones_ = key;
        begin_key_fade_locked(previous_key, key, output_frame);
        reset_stretch_locked();
        align_read_frame_to_next_output_locked();
        return true;
    }

double DirectSoundPcmAudioBackend::position() const
{
        // Non-blocking by design. The audio worker holds mutex_ while running
        // the pitch-shift DSP, so waiting here stalled the caller (the render
        // loop polls this for A/V drift) for 5-27 ms, which showed up on
        // screen as periodic judder. When the lock is busy, return the value
        // from the last successful read instead: the worker refreshes it every
        // few milliseconds, far finer than the 500 ms drift check that uses it.
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            return cached_position_.load(std::memory_order_relaxed);
        }
        if (!opened_) return 0.0;
        const auto frame = current_play_frame_locked();
        const auto seconds = std::min(duration(),
            static_cast<double>(frame) / active_pcm_->sample_rate);
        cached_position_.store(seconds, std::memory_order_relaxed);
        return seconds;
    }

double DirectSoundPcmAudioBackend::duration() const
{
        return active_pcm_->duration();
    }

bool DirectSoundPcmAudioBackend::is_open() const noexcept
{ return opened_; }

bool DirectSoundPcmAudioBackend::has_ended() const noexcept
{ return ended_.load(std::memory_order_relaxed); }

ProcessedPcmFormat DirectSoundPcmAudioBackend::processed_pcm_format() const
{
        return processed_pcm_ring_.format();
    }

std::size_t DirectSoundPcmAudioBackend::processed_pcm_available_frames() const
{
        return processed_pcm_ring_.available_frames();
    }

std::size_t DirectSoundPcmAudioBackend::read_processed_pcm(
        std::int16_t* output, std::size_t max_frames)
{
        return processed_pcm_ring_.read(output, max_frames);
    }

std::size_t DirectSoundPcmAudioBackend::read_processed_pcm_at(
        std::uint64_t timeline_start_frame,
        std::int16_t* output,
        std::size_t max_frames)
{
        return processed_pcm_ring_.read_at(
            timeline_start_frame, output, max_frames);
    }

bool DirectSoundPcmAudioBackend::create_directsound_buffer()
{
        auto hr = DirectSoundCreate8(nullptr, &direct_sound_, nullptr);
        if (FAILED(hr) || !direct_sound_) {
            log_hresult("DirectSoundCreate8 failed", hr);
            return false;
        }
        hr = direct_sound_->SetCooperativeLevel(
            GetDesktopWindow(), DSSCL_PRIORITY);
        if (FAILED(hr)) {
            log_hresult("SetCooperativeLevel(DirectSound) failed", hr);
            return false;
        }

        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = static_cast<WORD>(active_pcm_->channels);
        format.nSamplesPerSec = static_cast<DWORD>(active_pcm_->sample_rate);
        format.wBitsPerSample = 16;
        format.nBlockAlign =
            static_cast<WORD>(format.nChannels * (format.wBitsPerSample / 8));
        format.nAvgBytesPerSec =
            format.nSamplesPerSec * format.nBlockAlign;

        block_align_ = format.nBlockAlign;
        const auto target_buffer_bytes =
            static_cast<std::size_t>(format.nAvgBytesPerSec / 2);
        buffer_bytes_ = std::max<std::size_t>(
            target_buffer_bytes, static_cast<std::size_t>(block_align_) * 4096);
        buffer_bytes_ -= buffer_bytes_ % block_align_;

        DSBUFFERDESC desc{};
        desc.dwSize = sizeof(desc);
        desc.dwFlags = DSBCAPS_GLOBALFOCUS
            | DSBCAPS_GETCURRENTPOSITION2
            | DSBCAPS_CTRLVOLUME;
        desc.dwBufferBytes = static_cast<DWORD>(buffer_bytes_);
        desc.lpwfxFormat = &format;
        hr = direct_sound_->CreateSoundBuffer(&desc, &buffer_, nullptr);
        if (FAILED(hr) || !buffer_) {
            log_hresult("CreateSoundBuffer(DirectSound) failed", hr);
            return false;
        }
        hr = buffer_->SetVolume(
            local_output_enabled_ ? DSBVOLUME_MAX : DSBVOLUME_MIN);
        if (FAILED(hr)) {
            log_hresult("SetVolume(DirectSound output gate) failed", hr);
            return false;
        }
        clear_directsound_buffer();
        return true;
    }

void DirectSoundPcmAudioBackend::clear_directsound_buffer()
{
        if (!buffer_ || buffer_bytes_ == 0) return;
        void* ptr1{};
        void* ptr2{};
        DWORD bytes1{};
        DWORD bytes2{};
        if (SUCCEEDED(buffer_->Lock(0, static_cast<DWORD>(buffer_bytes_),
                &ptr1, &bytes1, &ptr2, &bytes2, 0))) {
            if (ptr1 && bytes1) std::memset(ptr1, 0, bytes1);
            if (ptr2 && bytes2) std::memset(ptr2, 0, bytes2);
            static_cast<void>(buffer_->Unlock(ptr1, bytes1, ptr2, bytes2));
        }
    }

void DirectSoundPcmAudioBackend::reset_stream_state(std::uint64_t frame)
{
        ++track_generation_;
        if (buffer_) {
            static_cast<void>(buffer_->Stop());
            static_cast<void>(buffer_->SetCurrentPosition(0));
            clear_directsound_buffer();
        }
        buffer_playing_ = false;
        stream_start_frame_ = std::min<std::uint64_t>(
            frame, static_cast<std::uint64_t>(active_pcm_->frame_count()));
        submitted_frames_ = 0;
        next_write_byte_ = 0;
        processed_pcm_ring_.clear();
        cancel_key_fade_locked();
        reset_stretch_locked();
        align_read_frame_to_next_output_locked();
    }

void DirectSoundPcmAudioBackend::reset_stretch_locked()
{
        stretch_configured_ = false;
        stretch_key_semitones_ = key_semitones_;
        stretch_latency_frames_ = 0;
    }

std::uint64_t DirectSoundPcmAudioBackend::next_output_frame_locked() const
{
        return stream_start_frame_ + submitted_frames_;
    }

std::uint64_t DirectSoundPcmAudioBackend::stretch_lookahead_frames_locked()
{
        if (key_semitones_ == 0 || !opened_
                || active_pcm_->sample_rate <= 0
                || active_pcm_->channels <= 0) {
            return 0;
        }
        configure_stretch_locked();
        return static_cast<std::uint64_t>(
            std::max(0, stretch_latency_frames_));
    }

void DirectSoundPcmAudioBackend::align_read_frame_to_next_output_locked()
{
        const auto output_frame = next_output_frame_locked();
        const auto lookahead = stretch_lookahead_frames_locked();
        read_frame_ = output_frame + lookahead;
        if (lookahead > 0) {
            prime_stretch_for_output_frame_locked(output_frame, lookahead);
        }
    }

void DirectSoundPcmAudioBackend::retarget_buffer_after_track_switch_locked(std::uint64_t play_frame)
{
        cancel_key_fade_locked();
        reset_stretch_locked();

        if (!playing_ || !buffer_ || !buffer_playing_ || block_align_ == 0
                || buffer_bytes_ == 0) {
            reset_stream_state(play_frame);
            return;
        }

        DWORD play_cursor{};
        DWORD write_cursor{};
        if (FAILED(buffer_->GetCurrentPosition(&play_cursor, &write_cursor))) {
            reset_stream_state(play_frame);
            return;
        }

        std::size_t write_offset = write_cursor;
        const auto align = static_cast<std::size_t>(block_align_);
        const auto misalignment = write_offset % align;
        if (misalignment != 0) {
            write_offset =
                (write_offset + align - misalignment) % buffer_bytes_;
        }

        const auto unsafe_bytes = ring_distance_bytes(
            play_cursor, write_offset, buffer_bytes_);
        const auto unsafe_frames =
            static_cast<std::uint64_t>(unsafe_bytes / block_align_);

        // next_output_frame must NOT be clamped to active_pcm_->frame_count().
        // Clamping here would set submitted_frames_ to a value smaller than
        // the actual playback position, causing position() to jump backward.
        // render_pcm_frames already gracefully handles reading past end-of-track
        // (zero-fill), and current_play_frame_locked() clamps on query.
        const auto next_output_frame = play_frame + unsafe_frames;

        if (next_output_frame < stream_start_frame_) {
            stream_start_frame_ = next_output_frame;
            submitted_frames_ = 0;
        } else {
            submitted_frames_ = next_output_frame - stream_start_frame_;
        }
        next_write_byte_ = write_offset;
        align_read_frame_to_next_output_locked();
    }

std::uint64_t DirectSoundPcmAudioBackend::current_play_frame_locked() const
{
        if (!buffer_ || block_align_ == 0 || buffer_bytes_ == 0) {
            return stream_start_frame_;
        }
        DWORD play_cursor{};
        DWORD write_cursor{};
        if (FAILED(buffer_->GetCurrentPosition(&play_cursor, &write_cursor))) {
            return std::min<std::uint64_t>(
                stream_start_frame_ + submitted_frames_,
                static_cast<std::uint64_t>(active_pcm_->frame_count()));
        }
        static_cast<void>(write_cursor);
        const auto queued_bytes = ring_distance_bytes(play_cursor,
            next_write_byte_, buffer_bytes_);
        const auto queued_frames =
            static_cast<std::uint64_t>(queued_bytes / block_align_);
        const auto played_frames = submitted_frames_ > queued_frames
            ? submitted_frames_ - queued_frames
            : 0;
        return std::min<std::uint64_t>(
            stream_start_frame_ + played_frames,
            static_cast<std::uint64_t>(active_pcm_->frame_count()));
    }

void DirectSoundPcmAudioBackend::render_loop()
{
        while (true) {
            {
                std::unique_lock lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(8), [this] {
                    return stop_thread_ || playing_;
                });
                if (stop_thread_) return;
                if (!playing_) continue;
            }

            fill_output_buffer();

            {
                std::lock_guard lock(mutex_);
                if (!playing_ || stop_thread_) continue;
                if (!buffer_playing_ && buffer_) {
                    if (SUCCEEDED(buffer_->Play(0, 0, DSBPLAY_LOOPING))) {
                        buffer_playing_ = true;
                    }
                }
                const auto play_frame = current_play_frame_locked();
                // Refresh the value position() falls back to when it cannot
                // take the lock. This runs every worker iteration (~8 ms), so
                // the fallback is never more than a few ms stale.
                if (opened_ && active_pcm_->sample_rate > 0) {
                    cached_position_.store(
                        std::min(duration(),
                            static_cast<double>(play_frame)
                                / active_pcm_->sample_rate),
                        std::memory_order_relaxed);
                }
                if (play_frame >= active_pcm_->frame_count()
                        && read_frame_ >= active_pcm_->frame_count()) {
                    playing_ = false;
                    if (buffer_) static_cast<void>(buffer_->Stop());
                    buffer_playing_ = false;
                    ended_.store(true, std::memory_order_relaxed);
                }
            }
        }
    }

void DirectSoundPcmAudioBackend::fill_output_buffer()
{
        while (true) {
            DWORD play_cursor{};
            DWORD write_cursor{};
            std::size_t write_offset{};
            std::size_t writable_bytes{};
            std::uint64_t gen;
            {
                std::lock_guard lock(mutex_);
                if (!playing_ || !buffer_ || block_align_ == 0
                        || buffer_bytes_ == 0) {
                    return;
                }
                if (FAILED(buffer_->GetCurrentPosition(
                        &play_cursor, &write_cursor))) {
                    return;
                }
                static_cast<void>(write_cursor);
                const auto queued_bytes = ring_distance_bytes(play_cursor,
                    next_write_byte_, buffer_bytes_);
                const auto safety_bytes = std::max<std::size_t>(
                        static_cast<std::size_t>(block_align_) * 256,
                    static_cast<std::size_t>(
                        active_pcm_->sample_rate * block_align_ / 100));
                if (queued_bytes + safety_bytes >= buffer_bytes_) return;
                writable_bytes = buffer_bytes_ - queued_bytes - safety_bytes;
                writable_bytes -= writable_bytes % block_align_;
                const auto max_chunk_bytes =
                    static_cast<std::size_t>(block_align_) * 2048;
                writable_bytes = std::min(writable_bytes, max_chunk_bytes);
                write_offset = next_write_byte_;
                gen = track_generation_.load();
            }
            if (writable_bytes == 0) return;

            const auto frames = writable_bytes / block_align_;
            std::vector<std::int16_t> pcm(frames
                * static_cast<std::size_t>(active_pcm_->channels));
            std::uint64_t timeline_start_frame{};
            render_pcm_frames(pcm.data(), frames, timeline_start_frame);

            void* ptr1{};
            void* ptr2{};
            DWORD bytes1{};
            DWORD bytes2{};
            if (FAILED(buffer_->Lock(static_cast<DWORD>(write_offset),
                    static_cast<DWORD>(writable_bytes),
                    &ptr1, &bytes1, &ptr2, &bytes2, 0))) {
                return;
            }
            auto* bytes = reinterpret_cast<const BYTE*>(pcm.data());
            if (ptr1 && bytes1) std::memcpy(ptr1, bytes, bytes1);
            if (ptr2 && bytes2) {
                std::memcpy(ptr2, bytes + bytes1, bytes2);
            }
            static_cast<void>(buffer_->Unlock(ptr1, bytes1, ptr2, bytes2));

            {
                std::lock_guard lock(mutex_);
                // If a track switch happened while we were rendering,
                // retarget_buffer_after_track_switch_locked has already
                // updated next_write_byte_ and submitted_frames_.
                // Discard our stale update to avoid position drift.
                if (track_generation_.load() != gen) return;
                next_write_byte_ =
                    (write_offset + writable_bytes) % buffer_bytes_;
                submitted_frames_ += static_cast<std::uint64_t>(frames);
            }
            processed_pcm_ring_.push_timed(
                pcm.data(), frames, timeline_start_frame);
        }
    }

void DirectSoundPcmAudioBackend::configure_stretch_locked()
{
        if (stretch_configured_
                && stretch_key_semitones_ == key_semitones_) {
            return;
        }
        stretch_latency_frames_ =
            configure_stretch_for_key_locked(stretch_, key_semitones_);
        stretch_configured_ = true;
        stretch_key_semitones_ = key_semitones_;
    }

int DirectSoundPcmAudioBackend::configure_stretch_for_key_locked( signalsmith::stretch::SignalsmithStretch<float>& stretcher, int key)
{
        stretcher.presetDefault(active_pcm_->channels,
            static_cast<float>(active_pcm_->sample_rate), true);
        stretcher.setTransposeSemitones(
            static_cast<float>(key),
            8000.0f / static_cast<float>(active_pcm_->sample_rate));
        stretcher.reset();
        return std::max(0, static_cast<int>(
            std::ceil(stretcher.outputSeekLength(1.0f))));
    }

void DirectSoundPcmAudioBackend::fill_planar_source_locked( std::uint64_t start_frame, std::size_t frames, std::vector<std::vector<float>>& planar, std::vector<float*>& ptrs)
{
        const auto channels = static_cast<std::size_t>(active_pcm_->channels);
        const auto total_frames = active_pcm_->frame_count();
        planar.assign(channels, std::vector<float>(frames, 0.0f));
        ptrs.resize(channels);
        for (std::size_t channel = 0; channel < channels; ++channel) {
            ptrs[channel] = planar[channel].data();
        }
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto source_frame = start_frame + frame;
            if (source_frame >= total_frames) continue;
            const auto src =
                static_cast<std::size_t>(source_frame) * channels;
            for (std::size_t channel = 0; channel < channels; ++channel) {
                planar[channel][frame] =
                    active_pcm_->samples[src + channel];
            }
        }
    }

void DirectSoundPcmAudioBackend::prime_stretch_for_output_frame_locked( std::uint64_t output_frame, std::uint64_t lookahead)
{
        if (!stretch_configured_ || key_semitones_ == 0 || lookahead == 0) {
            return;
        }
        const auto input_length =
            static_cast<int>(std::min<std::uint64_t>(
                lookahead, static_cast<std::uint64_t>(
                    std::numeric_limits<int>::max())));
        fill_planar_source_locked(output_frame,
            static_cast<std::size_t>(input_length),
            stretch_seek_input_, stretch_seek_input_ptrs_);
        stretch_.outputSeek(stretch_seek_input_ptrs_.data(), input_length);
    }

void DirectSoundPcmAudioBackend::render_pcm_frames(std::int16_t* output,
        std::size_t frames, std::uint64_t& timeline_start_frame)
{
        const auto channels = static_cast<std::size_t>(active_pcm_->channels);
        std::vector<float> processed(frames
            * channels);
        {
            std::lock_guard lock(mutex_);
            timeline_start_frame = next_output_frame_locked();
            if (key_semitones_ == 0) {
                render_plain_float_locked(processed.data(), frames);
            } else {
                configure_stretch_locked();
                render_shifted_float_locked(processed.data(), frames);
            }
            if (key_fade_active_) {
                const auto fade_frames = static_cast<std::size_t>(
                    std::min<std::uint64_t>(
                        static_cast<std::uint64_t>(frames),
                        key_fade_total_frames_ - key_fade_done_frames_));
                if (fade_frames > 0) {
                    std::vector<float> fade_from(fade_frames * channels);
                    render_key_fade_source_locked(fade_from.data(), fade_frames);
                    blend_key_fade_locked(
                        processed.data(), fade_from.data(), fade_frames);
                    key_fade_done_frames_ += fade_frames;
                }
                if (key_fade_done_frames_ >= key_fade_total_frames_) {
                    cancel_key_fade_locked();
                }
            }
            const auto volume =
                static_cast<float>(volume_percent_) / 100.0f;
            for (std::size_t i = 0; i < processed.size(); ++i) {
                const auto sample =
                    std::clamp(processed[i] * volume, -1.0f, 1.0f);
                output[i] = static_cast<std::int16_t>(
                    std::lrint(sample * 32767.0f));
            }
        }
    }

void DirectSoundPcmAudioBackend::render_plain_float_locked(float* output, std::size_t frames)
{
        render_plain_float_from_cursor_locked(output, frames, read_frame_);
    }

void DirectSoundPcmAudioBackend::render_plain_float_from_cursor_locked( float* output, std::size_t frames, std::uint64_t& read_frame)
{
        const auto channels = static_cast<std::size_t>(active_pcm_->channels);
        const auto total_frames = active_pcm_->frame_count();
        for (std::size_t frame = 0; frame < frames; ++frame) {
            if (read_frame < total_frames) {
                const auto src = static_cast<std::size_t>(read_frame) * channels;
                std::memcpy(output + frame * channels,
                    active_pcm_->samples.data() + src,
                    channels * sizeof(float));
                ++read_frame;
            } else {
                std::fill(output + frame * channels,
                    output + (frame + 1) * channels, 0.0f);
            }
        }
    }

void DirectSoundPcmAudioBackend::render_shifted_float_locked(float* output, std::size_t frames)
{
        render_shifted_float_from_cursor_locked(stretch_, output, frames,
            read_frame_, stretch_input_, stretch_output_, stretch_input_ptrs_,
            stretch_output_ptrs_);
    }

void DirectSoundPcmAudioBackend::render_shifted_float_from_cursor_locked( signalsmith::stretch::SignalsmithStretch<float>& stretcher, float* output, std::size_t frames, std::uint64_t& read_frame, std::vector<std::vector<float>>& input, std::vector<std::vector<float>>& stretched_output, std::vector<float*>& input_ptrs, std::vector<float*>& output_ptrs)
{
        const auto channels = static_cast<std::size_t>(active_pcm_->channels);
        input.assign(channels, std::vector<float>(frames, 0.0f));
        stretched_output.assign(channels, std::vector<float>(frames, 0.0f));
        input_ptrs.resize(channels);
        output_ptrs.resize(channels);
        const auto total_frames = active_pcm_->frame_count();
        for (std::size_t channel = 0; channel < channels; ++channel) {
            input_ptrs[channel] = input[channel].data();
            output_ptrs[channel] = stretched_output[channel].data();
        }
        for (std::size_t frame = 0; frame < frames; ++frame) {
            if (read_frame < total_frames) {
                const auto src = static_cast<std::size_t>(read_frame) * channels;
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    input[channel][frame] =
                        active_pcm_->samples[src + channel];
                }
                ++read_frame;
            }
        }
        stretcher.process(input_ptrs.data(), static_cast<int>(frames),
            output_ptrs.data(), static_cast<int>(frames));
        for (std::size_t frame = 0; frame < frames; ++frame) {
            for (std::size_t channel = 0; channel < channels; ++channel) {
                output[frame * channels + channel] =
                    stretched_output[channel][frame];
            }
        }
    }

void DirectSoundPcmAudioBackend::begin_key_fade_locked( int from_key, int to_key, std::uint64_t output_frame)
{
        cancel_key_fade_locked();
        if (!opened_ || from_key == to_key
                || active_pcm_->sample_rate <= 0
                || active_pcm_->channels <= 0) {
            return;
        }
        key_fade_total_frames_ = key_fade_frame_count_locked();
        if (key_fade_total_frames_ == 0) return;
        key_fade_from_key_ = from_key;
        key_fade_done_frames_ = 0;
        key_fade_plain_read_frame_ = output_frame;
        key_fade_shifted_read_frame_ = output_frame;
        if (from_key != 0) {
            const auto lookahead = static_cast<std::uint64_t>(
                configure_stretch_for_key_locked(
                    transition_stretch_, from_key));
            key_fade_shifted_read_frame_ = output_frame + lookahead;
            if (lookahead > 0) {
                prime_stretch_instance_for_output_frame_locked(
                    transition_stretch_, output_frame, lookahead,
                    key_fade_seek_input_, key_fade_seek_input_ptrs_);
            }
        }
        key_fade_active_ = true;
    }

void DirectSoundPcmAudioBackend::cancel_key_fade_locked()
{
        key_fade_active_ = false;
        key_fade_from_key_ = 0;
        key_fade_total_frames_ = 0;
        key_fade_done_frames_ = 0;
        key_fade_plain_read_frame_ = 0;
        key_fade_shifted_read_frame_ = 0;
    }

std::uint64_t DirectSoundPcmAudioBackend::key_fade_frame_count_locked() const
{
        if (active_pcm_->sample_rate <= 0) return 0;
        return std::max<std::uint64_t>(1,
            static_cast<std::uint64_t>(
                std::llround(active_pcm_->sample_rate * kKeyFadeSeconds)));
    }

void DirectSoundPcmAudioBackend::prime_stretch_instance_for_output_frame_locked( signalsmith::stretch::SignalsmithStretch<float>& stretcher, std::uint64_t output_frame, std::uint64_t lookahead, std::vector<std::vector<float>>& seek_input, std::vector<float*>& seek_input_ptrs)
{
        const auto input_length =
            static_cast<int>(std::min<std::uint64_t>(
                lookahead, static_cast<std::uint64_t>(
                    std::numeric_limits<int>::max())));
        if (input_length <= 0) return;
        fill_planar_source_locked(output_frame,
            static_cast<std::size_t>(input_length),
            seek_input, seek_input_ptrs);
        stretcher.outputSeek(seek_input_ptrs.data(), input_length);
    }

void DirectSoundPcmAudioBackend::render_key_fade_source_locked( float* output, std::size_t frames)
{
        if (key_fade_from_key_ == 0) {
            render_plain_float_from_cursor_locked(
                output, frames, key_fade_plain_read_frame_);
        } else {
            render_shifted_float_from_cursor_locked(
                transition_stretch_, output, frames,
                key_fade_shifted_read_frame_, key_fade_input_,
                key_fade_output_, key_fade_input_ptrs_,
                key_fade_output_ptrs_);
        }
    }

void DirectSoundPcmAudioBackend::blend_key_fade_locked( float* processed, const float* fade_from, std::size_t fade_frames)
{
        const auto channels = static_cast<std::size_t>(active_pcm_->channels);
        for (std::size_t frame = 0; frame < fade_frames; ++frame) {
            const auto fade_position =
                static_cast<double>(key_fade_done_frames_ + frame + 1)
                    / static_cast<double>(key_fade_total_frames_);
            const auto amount = std::clamp(fade_position, 0.0, 1.0);
            const auto from_gain = static_cast<float>(
                std::cos(amount * kHalfPi));
            const auto to_gain = static_cast<float>(
                std::sin(amount * kHalfPi));
            for (std::size_t channel = 0; channel < channels; ++channel) {
                const auto index = frame * channels + channel;
                processed[index] =
                    fade_from[index] * from_gain + processed[index] * to_gain;
            }
        }
    }
