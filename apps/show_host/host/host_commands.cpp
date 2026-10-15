// Mechanically split out of host_commands.h:
// the definitions below are unchanged, only their translation unit moved.

#include "host_commands.h"


  // defined below render_idle_frame

void render_idle_frame(ShowHost& host) {
    if (!has_ts_stage_output(host)) {
        service_native_stage_program(host);
        if (!native_stage_program_clock_is_active(host)) {
            host.playback.player.apply_pending_seek();
            host.playback.player.try_play_if_requested();
            maintain_video_master_sync(host);
        }
    }
    static_cast<void>(refresh_stage_program_ready(host));
    expire_stalled_program_transition(host);
    update_cached_position(host);
    const bool native_output_visible = has_physical_stage_output(host)
        || has_active_stage_texture_source(host)
        ;
    if (native_output_visible && !has_ts_stage_output(host)) {
        render_unified_native_stage(host);
    }
    if (host.native_stage.native_video_start_pending
            && (!native_output_visible || has_ts_stage_output(host))) {
        release_native_video_start_gate(host);
    } else if (host.native_stage.native_video_start_pending
            && !host.native_stage.native_output_recovery_pending
            && host.native_stage.native_video_start_started.time_since_epoch().count() != 0
            && std::chrono::steady_clock::now()
                - host.native_stage.native_video_start_started
                    >= kNativeVideoStartTimeout) {
        // Match Cast: this threshold reports an unusually slow source but is
        // never permission to advance audio over a black Stage. Rendering is
        // attempted first so a frame becoming ready on this tick wins.
        log_line("native Stage first frame still priming after 5 seconds");
        host.native_stage.native_video_start_started = {};
    }

}
