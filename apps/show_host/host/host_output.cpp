#include "host_output.h"

#include "host_layout.h"
#include "host_program.h"
#include "host_render.h"
#include "host_utils.h"

#include <algorithm>

namespace {

CastProgramBindings cast_program_bindings(ShowHost& host) {
    return CastProgramBindings{
        &host.playback.state,
        &host.playback.position,
        &host.playback.duration,
        &host.playback.video_buffering,
        &host.playback.video_buffering_position,
        host.window};
}

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

void rollback_cast_start(
        ShowHost& host,
        double restore_position,
        bool restore_playing) {
    static_cast<void>(stop_cast_pipeline(host.cast));
    host.output_transition.cast_video_source_active.store(
        false, std::memory_order_release);
    host.output_transition.cast_media_engine_detached = false;
    host.output_transition.native_video_suspended_for_cast = false;
    static_cast<void>(host.playback.audio->set_local_output_enabled(true));
    static_cast<void>(host.output_transition.stage_output_lease.release(
        StageOutputMode::ts_stream));
    if (host.native_stage.program_clock_active) {
        discontinuity_native_stage_program_clock(host, restore_position);
        if (restore_playing) {
            static_cast<void>(host.playback.audio->pause());
            host.native_stage.native_video_start_pending = true;
            host.native_stage.native_video_start_position = restore_position;
            host.native_stage.native_video_start_started =
                std::chrono::steady_clock::now();
            host.playback.video_buffering.store(true);
            host.playback.video_buffering_position.store(
                restore_position, std::memory_order_relaxed);
        }
    }
    apply_stage_layout(host);
}

}  // namespace

bool handle_start_cast_stream(ShowHost& host, std::uint16_t port) {
    const auto takeover_position = playback_time(host);
    const bool takeover_playing =
        host.playback.state.load() == SHOW_STATE_PLAYING;
    if (!host.output_transition.stage_output_lease.try_acquire(
            StageOutputMode::ts_stream)) {
        return false;
    }

    // Cast and native display profiles are mutually exclusive. Release every
    // retained native lease before rebuilding the common frame pool.
    release_unified_native_stage_frames(host);
    host.native_stage.stage_video_decoder.suspend();
    suspend_native_stage_standby_video(host, true);
    host.cast.program = cast_program_bindings(host);
    publish_current_cast_program(host);
    if (!start_cast_pipeline(host.cast, port)) {
        rollback_cast_start(host, takeover_position, takeover_playing);
        return false;
    }
    if (!host.playback.audio->set_local_output_enabled(false)) {
        log_line("cast stream: failed to close local audio output gate");
        rollback_cast_start(host, takeover_position, takeover_playing);
        return false;
    }

    const bool prime_current_video = takeover_playing
        && host.playback.current_media.loaded
        && !host.playback.current_media.video.empty();
    if (!prepare_cast_pipeline(
            host.cast, takeover_position, prime_current_video)) {
        rollback_cast_start(host, takeover_position, takeover_playing);
        return false;
    }

    // The worker is prepared but cannot render until MediaEngine has released
    // the source. This preserves the single Cast video-decoder owner rule.
    release_native_video_start_gate(host);
    host.playback.position.store(takeover_position);
    host.output_transition.cast_media_engine_detached =
        host.playback.current_media.loaded;
    host.output_transition.native_video_suspended_for_cast =
        host.output_transition.cast_media_engine_detached;
    if (host.output_transition.cast_media_engine_detached) {
        host.playback.player.close();
        host.output_transition.media_engine_video_source_active.store(
            false, std::memory_order_release);
    }
    host.output_transition.cast_video_source_active.store(
        host.playback.current_media.loaded
            && !host.playback.current_media.video.empty(),
        std::memory_order_release);
    host.output_transition.native_stage_video_source_active.store(
        false, std::memory_order_release);
    if (takeover_playing && host.playback.audio->is_open()) {
        static_cast<void>(host.playback.audio->pause());
    }

    host.render_loop_active = true;
    enable_cast_pipeline(host.cast);
    return true;
}

void handle_stop_cast_stream(ShowHost& host) {
    // Existing hosts call stop before every first Cast connection. Preserve
    // the idempotent contract: without the TS lease there is no Cast clock to
    // restore, and touching it would rewind normal playback to zero.
    if (!has_ts_stage_output(host)) return;

    const auto restore_state = host.playback.state.load();
    bool restore_native_program_clock =
        host.native_stage.program_clock_active
        && host.playback.current_media.loaded
        && !host.playback.current_media.video.empty()
        && host.playback.audio->is_open();
    const bool media_engine_was_detached =
        host.output_transition.cast_media_engine_detached
        && host.playback.current_media.loaded
        && !host.playback.current_media.video.empty();
    static_cast<void>(host.playback.audio->pause());
    static_cast<void>(host.playback.audio->set_local_output_enabled(true));
    const auto restore_position = stop_cast_pipeline(host.cast);
    host.output_transition.cast_video_source_active.store(
        false, std::memory_order_release);
    static_cast<void>(host.output_transition.stage_output_lease.release(
        StageOutputMode::ts_stream));
    host.output_transition.native_video_suspended_for_cast = false;
    host.output_transition.cast_media_engine_detached = false;
    host.playback.position.store(restore_position);
    host.playback.video_buffering.store(false);

    if (!restore_native_program_clock) {
        restore_native_program_clock =
            activate_native_stage_program_clock(host, restore_position);
    }

    if (restore_native_program_clock) {
        discontinuity_native_stage_program_clock(host, restore_position);
        host.output_transition.native_stage_video_source_active.store(
            true, std::memory_order_release);
    }

    const bool restore_media_engine =
        !restore_native_program_clock && media_engine_was_detached;
    bool media_engine_ready = !restore_media_engine;
    if (restore_media_engine) {
        media_engine_ready = open_media_engine_video(
            host, host.playback.current_media.video);
        if (!media_engine_ready) {
            log_line("stop cast: failed to restore MediaEngine video");
        }
    }
    apply_audio_processing(host);
    if (host.playback.audio->is_open()) {
        static_cast<void>(host.playback.audio->seek(std::max(
            0.0,
            restore_position
                - host.playback.audio_processing.clock_offset_seconds)));
    }
    if (restore_state == SHOW_STATE_PLAYING) {
        if (restore_native_program_clock) {
            host.native_stage.native_video_start_pending = true;
            host.native_stage.native_video_start_position = restore_position;
            host.native_stage.native_video_start_started =
                std::chrono::steady_clock::now();
            host.playback.video_buffering.store(true);
            host.playback.video_buffering_position.store(
                restore_position, std::memory_order_relaxed);
            static_cast<void>(host.playback.audio->pause());
        } else if (media_engine_ready && restore_media_engine) {
            host.playback.player.seek(restore_position);
            host.playback.player.play();
        }
        if (!restore_native_program_clock
                && (!host.playback.video_master_clock || !restore_media_engine
                    || host.playback.player.has_future_data())) {
            static_cast<void>(host.playback.audio->play());
        } else if (!restore_native_program_clock) {
            host.playback.video_buffering.store(true);
            host.playback.video_buffering_position.store(
                restore_position, std::memory_order_relaxed);
        }
    } else {
        host.playback.player.pause();
        if (restore_state == SHOW_STATE_PAUSED) {
            host.playback.paused_position = restore_position;
            host.playback.has_paused_position = true;
        }
    }

    apply_stage_layout(host);
    if (host.window && IsWindowVisible(host.window)) {
        InvalidateRect(host.window, nullptr, FALSE);
        UpdateWindow(host.window);
    }
}
