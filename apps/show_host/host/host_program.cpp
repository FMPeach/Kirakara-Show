// Shared program, playback and output-transition orchestration.
// Definitions were mechanically moved from host_commands.cpp.

#include "host_program.h"

#include "host_commands.h"

namespace {

void publish_current_cast_program(ShowHost& host) {
    publish_cast_program_snapshot(
        host.cast,
        host.playback.audio,
        host.playback.document,
        host.playback.config,
        host.playback.stage_overlay_state,
        host.playback.stage_overlay_revision.load(std::memory_order_acquire),
        host.playback.current_media.video,
        host.playback.current_media.loaded,
        host.playback.video_master_clock,
        host.playback.audio_processing.clock_offset_seconds,
        host.playback.video_timeline_revision.load(std::memory_order_acquire));
}

void publish_current_cast_timeline(ShowHost& host) {
    publish_cast_timeline_snapshot(
        host.cast,
        host.playback.audio_processing.clock_offset_seconds,
        host.playback.video_timeline_revision.load(std::memory_order_acquire));
}

}  // namespace

double clamp_audio_clock_offset(double seconds) {
    if (!std::isfinite(seconds)) return 0.0;
    return std::clamp(seconds, -2.0, 2.0);
}

void bump_playback_revision(ShowHost& host) {
    host.playback.playback_revision.fetch_add(1, std::memory_order_relaxed);
}

void bump_video_timeline_revision(ShowHost& host) {
    host.playback.video_timeline_revision.fetch_add(1, std::memory_order_relaxed);
}

bool handle_stage_overlay_state(
        ShowHost& host, const StageOverlayRequest& request) {
    if (!request.state.valid()
            || request.state.revision
                < host.playback.stage_overlay_revision.load(std::memory_order_relaxed)) {
        return false;
    }
    host.playback.stage_overlay_state = request.state;
    host.playback.stage_overlay_revision.store(
        request.state.revision, std::memory_order_release);
    publish_cast_overlay_snapshot(
        host.cast, request.state, request.state.revision);
    return true;
}

void begin_program_transition(ShowHost& host, const LoadRequest& request) {
    static_cast<void>(request);
    // Every switch (hard or seamless) holds the previous complete frame until
    // the new program publishes its first frame, matching Cast behaviour.
    // native_presentation_holds_frame (presentation layer) is intentionally
    // NOT cleared here: it survives the transition and keeps the old frame on
    // screen while the new decoder opens.
    const bool native_program_visible =
        host.native_stage.native_presentation_holds_frame
        || host.output_transition.native_transition_holding;
    const bool cast_program_visible = has_ts_stage_output(host)
        && (host.cast.lifecycle.snapshot().program_ready
            || host.cast.lifecycle.snapshot().holding_previous_output);

    host.output_transition.native_transition_holding = native_program_visible;
    host.cast.lifecycle.begin_program_transition(cast_program_visible);
    host.output_transition.transition_hold_started =
        host.output_transition.native_transition_holding || cast_program_visible
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    // Each new hold cycle may log one takeover warning again.
    host.output_transition.native_transition_warned = false;
    host.native_stage.stage_program_ready = false;
    host.native_stage.stage_video_frame_available = false;
    clear_current_native_video_program(host);
    clear_native_video_start_gate(host);
    clear_cast_video_start_gate(host.cast);
    if (!host.output_transition.native_transition_holding) {
        ensure_stage_surface_visible(host);
    }
}

void fail_program_transition(ShowHost& host) {
    host.output_transition.native_transition_holding = false;
    host.output_transition.transition_hold_started = {};
    host.native_stage.stage_program_ready = false;
    host.native_stage.stage_video_frame_available = false;
    clear_current_native_video_program(host);
    clear_native_video_start_gate(host);
    clear_cast_video_start_gate(host.cast);
    host.cast.lifecycle.fail_program_transition();
    host.native_stage.native_presentation_holds_frame = false;
    ensure_stage_surface_visible(host);
}

void expire_stalled_program_transition(ShowHost& host) {
    if (!host.output_transition.native_transition_holding
            || host.output_transition.transition_hold_started.time_since_epoch().count() == 0) {
        return;
    }
    const auto elapsed = std::chrono::steady_clock::now()
        - host.output_transition.transition_hold_started;
    // Normal takeover window: keep the previous frame visible.
    if (elapsed < kSeamlessTransitionHold) return;
    // Past the seamless window: warn once per hold cycle and keep holding.
    // Normal decoder cold starts (external HTTP sources) exceed 750 ms and
    // must not turn into black. Only a much longer stall (aligned with the
    // first-frame gate) falls back to black so a broken source cannot freeze
    // forever. The warning is deliberately one-shot: log_line() flushes to
    // disk synchronously, so logging every frame would stall the render loop.
    if (elapsed < kNativeVideoStartTimeout) {
        if (!host.output_transition.native_transition_warned) {
            host.output_transition.native_transition_warned = true;
            log_line("native transition exceeded takeover window; holding last frame");
        }
        return;
    }
    host.output_transition.native_transition_holding = false;
    host.native_stage.native_presentation_holds_frame = false;
    ensure_stage_surface_visible(host);
    if (!host.cast.lifecycle.snapshot().holding_previous_output) {
        host.output_transition.transition_hold_started = {};
    }
    log_line("native seamless transition missed takeover window; showing black");
}

void apply_audio_processing(ShowHost& host) {
    const auto volume = std::clamp(host.playback.audio_processing.volume_percent, 0, 100);
    const auto key = std::clamp(host.playback.audio_processing.key_semitones, -6, 6);
    host.playback.audio_processing.volume_percent = volume;
    host.playback.audio_processing.key_semitones = key;
    host.playback.audio_processing.clock_offset_seconds =
        clamp_audio_clock_offset(host.playback.audio_processing.clock_offset_seconds);
    static_cast<void>(host.playback.audio->set_volume(volume));
    static_cast<void>(host.playback.audio->set_key_semitones(key));
    if (host.playback.audio->is_open()) {
        host.playback.player.set_volume(0, false);
    } else {
        host.playback.player.set_volume(volume, true);
    }
}

bool open_media_engine_video(
        ShowHost& host, const std::wstring& source) {
    const auto video_output_window = host.media_clock_window
        ? host.media_clock_window : host.window;
    if (!host.playback.player.open(host.window, video_output_window, source)) {
        host.output_transition.media_engine_video_source_active.store(
            false, std::memory_order_release);
        return false;
    }
    RECT rect{};
    GetClientRect(host.playback.player.playback_window, &rect);
    host.playback.player.resize(rect);
    host.output_transition.media_engine_video_source_active.store(
        !source.empty(), std::memory_order_release);
    return true;
}

bool same_load_request(
        const LoadRequest& left, const LoadRequest& right) {
    return left.video == right.video
        && left.lyric == right.lyric
        && left.vocal == right.vocal
        && left.accompaniment == right.accompaniment
        && left.clock_mode == right.clock_mode;
}

bool handle_prepare_next(ShowHost& host, const LoadRequest& request) {
    auto state = host.playback.prepared_media;
    if (!state) return false;

    std::uint64_t generation{};
    std::lock_guard worker_lock(state->mutex);
    if (!state->alive.load()) return false;
    generation = state->generation.fetch_add(1) + 1;
    state->ready.reset();
    state->workers.emplace_back([state, request, generation] {
        const auto com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto prepared = std::make_unique<PreparedMediaPayload>();
        prepared->request = request;
        prepared->prepare_generation = generation;
        prepared->lyric_ready = parse_lyric_project_data(
            request.lyric, prepared->document, prepared->config);

        const bool needs_audio = !request.vocal.empty()
            || !request.accompaniment.empty();
        if (!needs_audio) {
            prepared->audio_ready = true;
        } else {
            auto audio = std::make_unique<AudioPlayer>();
            if (!request.vocal.empty()) {
                prepared->audio_ready = audio->open_both(
                    request.vocal, request.accompaniment);
                prepared->audio_track = SHOW_AUDIO_TRACK_VOCAL;
            } else {
                prepared->audio_ready = audio->open(
                    request.accompaniment);
                prepared->audio_track = SHOW_AUDIO_TRACK_ACCOMPANIMENT;
            }
            if (prepared->audio_ready) {
                static_cast<void>(audio->set_local_output_enabled(false));
                prepared->audio = std::move(audio);
            }
        }

        if (state->alive.load()
                && state->generation.load() == generation) {
            std::lock_guard lock(state->mutex);
            if (state->alive.load()
                    && state->generation.load() == generation) {
                state->ready = std::move(prepared);
            }
        }
        if (SUCCEEDED(com_result)) CoUninitialize();
    });
    request_native_stage_standby_video(
        host, request.video, generation);
    return true;
}

std::unique_ptr<PreparedMediaPayload> take_prepared_media(
        ShowHost& host, const LoadRequest& request) {
    auto state = host.playback.prepared_media;
    if (!state) return {};
    state->generation.fetch_add(1);
    std::lock_guard lock(state->mutex);
    if (!state->ready
            || !same_load_request(state->ready->request, request)) {
        state->ready.reset();
        return {};
    }
    return std::move(state->ready);
}

bool handle_load(ShowHost& host, const LoadRequest& request) {
    auto prepared = take_prepared_media(host, request);
    const auto prepare_generation = prepared
        ? prepared->prepare_generation : 0;
    // Keep Cast transport fed during synchronous lyric/audio replacement.
    // MediaEngine is deliberately not recreated while Cast owns the output;
    // the Cast SourceReader is the sole video decoder in that interval.
    const bool load_keepalive_active = begin_cast_load_keepalive(host.cast);
    auto previous_audio = std::move(host.playback.audio);
    host.playback.audio = std::make_shared<AudioPlayer>();
    // With no Cast frame in flight the old backend can be released now. During
    // Cast its published snapshot owns the old player until that frame ends.
    if (!load_keepalive_active && previous_audio) previous_audio->close();
    close_video_outputs(host);
    begin_program_transition(host, request);
    host.native_stage.stage_video_decoder.suspend();
    host.output_transition.cast_video_source_active.store(false, std::memory_order_release);
    host.playback.current_media.clear();
    // No published source any more, so this best-effort suspend cannot leave
    // the worker holding a stale program if its bounded wait expires.
    suspend_cast_video(host.cast);
    host.playback.state.store(SHOW_STATE_IDLE);
    host.playback.position.store(0.0);
    host.playback.duration.store(0.0);
    host.playback.vocal_path = request.vocal;
    host.playback.accompaniment_path = request.accompaniment;
    host.playback.audio_track = SHOW_AUDIO_TRACK_VOCAL;
    host.playback.paused_position = 0.0;
    host.playback.has_paused_position = false;
    host.playback.video_master_clock =
        request.clock_mode == SHOW_CLOCK_VIDEO_MASTER;
    host.playback.video_buffering.store(false);
    host.playback.video_buffering_position.store(
        0.0, std::memory_order_relaxed);
    host.playback.last_audio_sync = {};

    const bool lyric_ready = prepared && prepared->lyric_ready;
    if (lyric_ready) {
        host.playback.document = std::move(prepared->document);
        host.playback.config = std::move(prepared->config);
    } else if (!parse_lyric_project(host, request.lyric)) {
        log_line("handle_load: parse_lyric_project failed");
        suspend_native_stage_standby_video(host, false);
        fail_program_transition(host);
        host.render_loop_active = false;
        host.playback.state.store(SHOW_STATE_IDLE);
        if (load_keepalive_active) publish_current_cast_program(host);
        end_cast_load_keepalive(host.cast, load_keepalive_active);
        return false;
    }
    const bool cast_owns_video = has_ts_stage_output(host);
    if (!cast_owns_video && !open_media_engine_video(host, request.video)) {
        log_line("handle_load: player.open failed");
        suspend_native_stage_standby_video(host, false);
        fail_program_transition(host);
        host.render_loop_active = false;
        host.playback.state.store(SHOW_STATE_IDLE);
        if (load_keepalive_active) publish_current_cast_program(host);
        end_cast_load_keepalive(host.cast, load_keepalive_active);
        return false;
    }
    host.output_transition.cast_media_engine_detached = cast_owns_video;

    if (prepared && prepared->audio_ready) {
        if (prepared->audio) {
            host.playback.audio = std::shared_ptr<AudioPlayer>(
                std::move(prepared->audio));
        }
        host.playback.audio_track = prepared->audio_track;
    } else if (!host.playback.vocal_path.empty()) {
        static_cast<void>(host.playback.audio->open_both(
            host.playback.vocal_path, host.playback.accompaniment_path));
    } else if (!host.playback.accompaniment_path.empty()) {
        host.playback.audio_track = SHOW_AUDIO_TRACK_ACCOMPANIMENT;
        static_cast<void>(
            host.playback.audio->open(host.playback.accompaniment_path));
    }

    apply_audio_processing(host);
    static_cast<void>(host.playback.audio->set_local_output_enabled(
        !has_ts_stage_output(host)));
    host.playback.current_media.publish(request);
    if (cast_owns_video) {
        host.output_transition.cast_video_source_active.store(
            !request.video.empty(), std::memory_order_release);
        host.cast.program_clock.discontinuity(
            0.0, cast_monotonic_seconds());
    }
    host.playback.state.store(SHOW_STATE_STOPPED);
    bump_video_timeline_revision(host);
    const auto video_timeline_revision =
        host.playback.video_timeline_revision.load(std::memory_order_relaxed);
    const bool promoted_native_video = prepare_generation != 0
        && promote_native_stage_standby_video(
            host,
            request.video,
            prepare_generation,
            video_timeline_revision);
    if (prepare_generation != 0
            && !cast_owns_video
            && !request.video.empty()
            && !promoted_native_video) {
        host.native_stage.pipeline_diagnostics.increment(
            NativeStagePipelineCounter::standby_fallbacks);
    }
    if (!promoted_native_video) {
        suspend_native_stage_standby_video(host, false);
    }
    bump_playback_revision(host);
    if (cast_owns_video) {
        publish_current_cast_program(host);
        refresh_cast_render_configuration(host.cast);
    }
    host.render_loop_active = true;
    static_cast<void>(
        set_stage_visible(host, has_physical_stage_output(host)));
    static_cast<void>(prime_native_stage_video(host, 0.0));
    // No Cast priming round trip here: the Cast frame loop synchronizes the
    // decoder against the published program on every frame, and waiting for
    // that handoff inside load() is what froze the UI on slow sources.
    end_cast_load_keepalive(host.cast, load_keepalive_active);

    return true;
}

bool refresh_stage_program_ready(ShowHost& host) {
    if (host.native_stage.stage_program_ready) return true;
    const auto state = host.playback.state.load();
    const bool video_frame_ready = host.native_stage.stage_video_frame_available;
    if (!host.playback.current_media.loaded
            || host.playback.video_buffering.load()
            || (host.output_transition.native_transition_holding
                && !video_frame_ready)
            || (state != SHOW_STATE_PLAYING
                && state != SHOW_STATE_PAUSED)
            || !video_frame_ready) {
        return false;
    }

    host.native_stage.stage_video_frame_available = true;
    host.native_stage.stage_program_ready = true;
    host.output_transition.native_transition_holding = false;
    ensure_stage_surface_visible(host);
    // The first MediaEngine present after a cold load can move the video HWND
    return true;
}

void handle_play(ShowHost& host) {
    const auto state = host.playback.state.load();
    const bool resume_from_pause =
        state == SHOW_STATE_PAUSED && host.playback.has_paused_position;
    const auto requested_start_position = resume_from_pause
        ? host.playback.paused_position
        : host.playback.position.load();
    // A load explicitly publishes position=0, while a stopped seek updates
    // host.playback.position to the requested start. Do not sample media_time() here:
    // the non-blocking audio clock may still expose a cached value from the
    // previous program while a newly opened backend is settling.
    const auto start_position =
        std::isfinite(requested_start_position)
            && requested_start_position >= 0.0
        ? requested_start_position
        : 0.0;
    const bool wait_for_native_video = !host.native_stage.stage_video_frame_available
        && prime_native_stage_video(host, start_position);
    // Cast handoff: hold the program clock (and the audio clock that drives
    // it in audio-master mode) until the cast decoder has its first frame.
    // Otherwise the decoder's session-open latency advances stage_time and
    // both the first video frame pick and the cast audio cursor start mid-
    // song. Transport PTS is unaffected; only program-internal time is gated.
    const auto video_revision =
        host.playback.video_timeline_revision.load(std::memory_order_relaxed);
    const bool current_cast_program_ready =
        host.cast.lifecycle.program_ready_for(video_revision);
    const bool wait_for_cast_video = has_ts_stage_output(host)
        && host.playback.current_media.loaded
        && !host.playback.current_media.video.empty()
        && !current_cast_program_ready;
    host.native_stage.native_video_start_pending = wait_for_native_video;
    host.native_stage.native_video_start_position = wait_for_native_video
        ? start_position : 0.0;
    host.native_stage.native_video_start_started = wait_for_native_video
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    host.native_stage.native_video_start_diagnostics_qpc =
        wait_for_native_video
            && host.native_stage.pipeline_diagnostics.enabled()
        ? NativeStagePipelineDiagnostics::qpc_now() : 0;
    host.cast.lifecycle.request_play(
        wait_for_cast_video,
        start_position,
        std::chrono::steady_clock::now());
    if (has_ts_stage_output(host)) {
        // A user pause does not change source identity or timeline. Preserve
        // the paused Cast clock and decoder queue so resume can use their
        // existing runway; cold starts, seeks and loads still reset through
        // the discontinuity path.
        if (!resume_from_pause) {
            host.cast.program_clock.discontinuity(
                start_position, cast_monotonic_seconds());
        }
        if (host.playback.audio->is_open()) {
            static_cast<void>(host.playback.audio->pause());
        }
        pause_video_outputs(host);
        host.playback.video_buffering.store(wait_for_cast_video);
        host.playback.video_buffering_position.store(
            start_position, std::memory_order_relaxed);
    } else if (host.playback.audio->is_open()) {
        static_cast<void>(host.playback.audio->seek(start_position));
        seek_video_outputs(host, start_position);
        if (wait_for_native_video || wait_for_cast_video) {
            static_cast<void>(host.playback.audio->pause());
            pause_video_outputs(host);
            host.playback.video_buffering.store(false);
        } else if (!host.playback.video_master_clock || host.playback.player.has_future_data()) {
            static_cast<void>(host.playback.audio->play());
            host.playback.video_buffering.store(false);
        } else {
            host.playback.video_buffering.store(true);
            host.playback.video_buffering_position.store(
                start_position, std::memory_order_relaxed);
        }
    } else {
        if (resume_from_pause) {
            seek_video_outputs(host, start_position);
        }
    }
    if (!wait_for_native_video && !wait_for_cast_video) {
        play_video_outputs(host);
    }
    host.playback.has_paused_position = false;
    host.playback.state.store(SHOW_STATE_PLAYING);
    static_cast<void>(refresh_stage_program_ready(host));
    bump_playback_revision(host);
}

void handle_pause(ShowHost& host) {
    const auto position = media_time(host);
    host.playback.paused_position = position;
    host.playback.has_paused_position = true;
    if (has_ts_stage_output(host)) {
        // Make the clock transition synchronous with the command. This also
        // keeps a rapid pause/play pair from observing the old playing state
        // before the next Cast render tick.
        host.cast.program_clock.pause_at(
            position, cast_monotonic_seconds());
    }
    host.playback.video_buffering.store(false);
    clear_native_video_start_gate(host);
    clear_cast_video_start_gate(host.cast);
    host.cast.lifecycle.request_pause();
    host.playback.state.store(SHOW_STATE_PAUSED);
    static_cast<void>(host.playback.audio->pause());
    pause_video_outputs(host);
    host.playback.position.store(playback_time(host));
    bump_playback_revision(host);
}

void handle_stop(ShowHost& host) {
    host.playback.state.store(SHOW_STATE_STOPPED);
    static_cast<void>(host.playback.audio->stop());
    pause_video_outputs(host);
    host.native_stage.stage_program_ready = false;
    host.native_stage.stage_video_frame_available = false;
    clear_current_native_video_program(host);
    clear_native_video_start_gate(host);
    clear_cast_video_start_gate(host.cast);
    host.output_transition.native_transition_holding = false;
    host.output_transition.transition_hold_started = {};
    host.cast.lifecycle.reset_program();
    host.native_stage.native_presentation_holds_frame = false;
    ensure_stage_surface_visible(host);
    seek_video_outputs(host, 0.0);
    host.playback.position.store(0.0);
    host.playback.paused_position = 0.0;
    host.playback.has_paused_position = false;
    host.playback.video_buffering.store(false);
    if (has_ts_stage_output(host)) {
        host.cast.program_clock.discontinuity(
            0.0, cast_monotonic_seconds());
    }
    bump_video_timeline_revision(host);
    publish_current_cast_timeline(host);
    bump_playback_revision(host);
}

void handle_media_ended(ShowHost& host) {
    const auto ended_position = playback_time(host);
    const bool native_program_visible =
        (host.native_stage.stage_program_ready && host.native_stage.stage_video_frame_available)
        || has_current_native_video_program(host);
    const bool cast_program_visible =
        has_ts_stage_output(host)
        && host.cast.lifecycle.snapshot().program_ready;

    host.playback.state.store(SHOW_STATE_STOPPED);
    static_cast<void>(host.playback.audio->stop());
    pause_video_outputs(host);
    host.output_transition.native_transition_holding = native_program_visible;
    host.cast.lifecycle.set_previous_output_hold(cast_program_visible);
    host.output_transition.transition_hold_started =
        native_program_visible || cast_program_visible
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    // Each new hold cycle (including media end) may log one takeover warning.
    host.output_transition.native_transition_warned = false;
    host.native_stage.stage_program_ready = false;
    host.native_stage.stage_video_frame_available = false;
    clear_native_video_start_gate(host);
    clear_cast_video_start_gate(host.cast);
    if (!native_program_visible) {
        host.native_stage.native_presentation_holds_frame = false;
        ensure_stage_surface_visible(host);
    }
    host.playback.position.store(ended_position);
    host.playback.paused_position = 0.0;
    host.playback.has_paused_position = false;
    host.playback.video_buffering.store(false);
    bump_playback_revision(host);
}

void handle_video_waiting(ShowHost& host) {
    if (has_ts_stage_output(host)) return;
    if (host.playback.state.load() != SHOW_STATE_PLAYING) {
        return;
    }
    if (host.native_stage.native_video_start_pending) return;
    if (host.playback.video_master_clock && host.playback.audio->is_open()) {
        if (host.playback.video_buffering.load()) return;
        host.playback.video_buffering_position.store(
            host.playback.player.current_time(), std::memory_order_relaxed);
        host.playback.video_buffering.store(true);
        static_cast<void>(host.playback.audio->pause());
        host.playback.position.store(
            host.playback.video_buffering_position.load(
                std::memory_order_relaxed));

        // A network stall is a program pause, not a source transition. Keep
        // the last complete video + lyric frame when one has already aired.
        if ((host.native_stage.stage_program_ready
                    && host.native_stage.stage_video_frame_available)
                || has_current_native_video_program(host)) {
            return;
        }
    }
    if (has_current_native_video_program(host)) return;
    if (host.output_transition.native_transition_holding) return;
    host.native_stage.stage_program_ready = false;
    host.native_stage.stage_video_frame_available = false;
    host.native_stage.native_presentation_holds_frame = false;
    ensure_stage_surface_visible(host);
}

void handle_video_can_play(ShowHost& host) {
    if (has_ts_stage_output(host)) return;
    if (host.native_stage.native_video_start_pending) return;
    if (!host.playback.video_master_clock || !host.playback.video_buffering.load()
            || !host.playback.audio->is_open()) {
        return;
    }
    if (host.playback.state.load() != SHOW_STATE_PLAYING) {
        host.playback.video_buffering.store(false);
        return;
    }
    if (!host.playback.player.has_future_data()) return;

    const auto video_position = host.playback.player.current_time();
    static_cast<void>(host.playback.audio->seek(video_position));
    static_cast<void>(host.playback.audio->play());
    host.playback.video_buffering.store(false);
    host.playback.last_audio_sync = std::chrono::steady_clock::now();
    static_cast<void>(refresh_stage_program_ready(host));
}

void maintain_video_master_sync(ShowHost& host) {
    if (has_ts_stage_output(host)) return;
    if (host.native_stage.native_video_start_pending) return;
    if (!host.playback.video_master_clock || !host.playback.audio->is_open()
            || host.playback.state.load() != SHOW_STATE_PLAYING) {
        return;
    }
    if (!host.playback.player.has_future_data()) {
        handle_video_waiting(host);
        return;
    }
    if (host.playback.video_buffering.load()) {
        handle_video_can_play(host);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (host.playback.last_audio_sync.time_since_epoch().count() != 0
            && now - host.playback.last_audio_sync < std::chrono::milliseconds(500)) {
        return;
    }
    host.playback.last_audio_sync = now;
    const auto video_position = host.playback.player.current_time();
    const auto audio_position = host.playback.audio->position();
    if (std::isfinite(video_position) && std::isfinite(audio_position)
            && std::abs(video_position - audio_position) > 0.20) {
        static_cast<void>(host.playback.audio->seek(video_position));
        static_cast<void>(host.playback.audio->play());
    }
}

void handle_seek(ShowHost& host, double seconds) {
    clear_native_video_start_gate(host);
    clear_cast_video_start_gate(host.cast);
    const bool was_playing = host.playback.state.load() == SHOW_STATE_PLAYING;
    if (was_playing) {
        host.output_transition.native_transition_holding = false;
        host.cast.lifecycle.set_previous_output_hold(false);
        host.output_transition.transition_hold_started = {};
        host.native_stage.stage_program_ready = false;
        host.native_stage.stage_video_frame_available = false;
        host.native_stage.native_presentation_holds_frame = false;
        ensure_stage_surface_visible(host);
    }
    const bool cast_active = has_ts_stage_output(host);
    if (cast_active) {
        host.cast.program_clock.discontinuity(
            seconds, cast_monotonic_seconds());
        static_cast<void>(host.playback.audio->seek(std::max(0.0,
            seconds - host.playback.audio_processing.clock_offset_seconds)));
    } else {
        static_cast<void>(host.playback.audio->seek(seconds));
    }
    seek_video_outputs(host, seconds);
    if (was_playing) {
        if (cast_active) {
            static_cast<void>(host.playback.audio->pause());
            pause_video_outputs(host);
            const bool wait_for_video = host.playback.current_media.loaded
                && !host.playback.current_media.video.empty();
            host.cast.lifecycle.request_play(
                wait_for_video,
                seconds,
                std::chrono::steady_clock::now());
            host.playback.video_buffering.store(wait_for_video);
            host.playback.video_buffering_position.store(
                seconds, std::memory_order_relaxed);
        } else if (host.playback.video_master_clock && host.playback.audio->is_open()
                && !host.playback.player.has_future_data()) {
            play_video_outputs(host);
            host.playback.video_buffering.store(true);
            host.playback.video_buffering_position.store(
                seconds, std::memory_order_relaxed);
        } else {
            play_video_outputs(host);
            static_cast<void>(host.playback.audio->play());
            host.playback.video_buffering.store(false);
        }
    }
    host.playback.position.store(seconds);
    if (host.playback.state.load() == SHOW_STATE_PAUSED) {
        host.playback.paused_position = seconds;
        host.playback.has_paused_position = true;
    }
    bump_video_timeline_revision(host);
    publish_current_cast_timeline(host);
    bump_playback_revision(host);
}

bool handle_audio_track(ShowHost& host, int track) {
    const auto target_track = track == SHOW_AUDIO_TRACK_ACCOMPANIMENT
        ? SHOW_AUDIO_TRACK_ACCOMPANIMENT
        : SHOW_AUDIO_TRACK_VOCAL;
    if (target_track == SHOW_AUDIO_TRACK_ACCOMPANIMENT
            && host.playback.accompaniment_path.empty()) {
        return false;
    }
    if (target_track == SHOW_AUDIO_TRACK_VOCAL && host.playback.vocal_path.empty()) {
        return false;
    }
    if (!host.playback.audio->set_track(target_track)) return false;
    host.playback.audio_track = track == SHOW_AUDIO_TRACK_ACCOMPANIMENT
        ? SHOW_AUDIO_TRACK_ACCOMPANIMENT
        : SHOW_AUDIO_TRACK_VOCAL;
    apply_audio_processing(host);
    bump_playback_revision(host);
    return true;
}

void handle_volume(ShowHost& host, int volume_percent) {
    host.playback.audio_processing.volume_percent = volume_percent;
    apply_audio_processing(host);
    bump_playback_revision(host);
}

void handle_key(ShowHost& host, int key_semitones) {
    host.playback.audio_processing.key_semitones = key_semitones;
    apply_audio_processing(host);
    bump_playback_revision(host);
}

void handle_audio_clock_offset(ShowHost& host, double seconds) {
    const auto cast_position = host.cast.program_clock.position_seconds();
    host.playback.audio_processing.clock_offset_seconds =
        clamp_audio_clock_offset(seconds);
    publish_current_cast_timeline(host);
    if (has_ts_stage_output(host) && host.playback.audio->is_open()) {
        // Offset changes do not change video identity or program time. Move
        // the slave audio cursor underneath the stable Cast clock instead of
        // forcing the decoder through a cold-start high-water gate.
        static_cast<void>(host.playback.audio->seek(std::max(0.0,
            cast_position - host.playback.audio_processing.clock_offset_seconds)));
        if (host.playback.state.load() == SHOW_STATE_PLAYING
                && host.cast.program_clock.state()
                    == CastProgramClockState::playing) {
            static_cast<void>(host.playback.audio->play());
        }
    }
    update_cached_position(host);
    bump_playback_revision(host);
}
