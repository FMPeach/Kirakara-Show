// Mechanically split out of host_render.h:
// the definitions below are unchanged, only their translation unit moved.

#include "host_render.h"


bool parse_lyric_project_data(
        const std::wstring& path,
        kirakara::show::PreparedDocument& document,
        kirakara::show::AppConfig& config) {
    if (path.empty()) {
        config = kirakara::show::default_app_config();
        document = {};
        return true;
    }
    auto raw = read_file(path);
    if (raw.empty()) return false;
    auto parsed = kirakara::show::parse_project_text(raw);
    document = std::move(parsed.document);
    config = std::move(parsed.config);
    return true;
}


bool parse_lyric_project(ShowHost& host, const std::wstring& path) {
    return parse_lyric_project_data(
        path, host.playback.document, host.playback.config);
}



double media_time(const ShowHost& host) {
    if (has_ts_stage_output(host)) {
        // Cast publishes its program position atomically from its worker; the
        // host query stays lock-free even though CastProgramClock itself also
        // protects low-frequency control-thread transitions.
        return host.playback.position.load(std::memory_order_relaxed);
    }
    if (host.playback.video_master_clock) return host.playback.player.current_time();
    if (host.playback.audio->is_open()) return host.playback.audio->position();
    return host.playback.player.current_time();
}



double playback_time(const ShowHost& host) {
    if (has_ts_stage_output(host)) {
        return host.playback.position.load(std::memory_order_relaxed);
    }
    const auto time = host.playback.video_buffering.load()
        ? host.playback.video_buffering_position.load(std::memory_order_relaxed)
        : media_time(host);
    if (!host.playback.audio->is_open()) return time;
    return std::max(0.0,
        time + host.playback.audio_processing.clock_offset_seconds);
}



void seek_video_outputs(ShowHost& host, double seconds) {
    host.playback.player.seek(seconds);
}



bool should_suspend_native_video_for_cast(const ShowHost& host) {
    // Cast owns its own video-gated program clock. MediaEngine must never be
    // played while the TS output lease is active, including the public
    // SHOW_CLOCK_VIDEO_MASTER mode; otherwise the same source is decoded by
    // both MediaEngine and the Cast SourceReader.
    return has_ts_stage_output(host);
}



void play_video_outputs(ShowHost& host) {
    if (should_suspend_native_video_for_cast(host)) {
        host.playback.player.pause();
        host.output_transition.native_video_suspended_for_cast = true;
        return;
    }
    host.output_transition.native_video_suspended_for_cast = false;
    host.playback.player.play();
}



void pause_video_outputs(ShowHost& host) {
    host.playback.player.pause();
}



void close_video_outputs(ShowHost& host) {
    host.output_transition.native_video_suspended_for_cast = false;
    host.playback.player.close();
    host.output_transition.media_engine_video_source_active.store(
        false, std::memory_order_release);
}



void update_cached_position(ShowHost& host) {
    const auto position = playback_time(host);
    host.playback.position.store(position);
    // MediaEngine is deliberately closed while Cast owns the Stage. Its
    // duration is therefore zero; do not transiently replace the decoder's
    // video duration with the auxiliary audio duration before the Cast tick
    // publishes the authoritative value again.
    if (has_ts_stage_output(host)) return;
    const auto audio_duration = host.playback.audio->duration();
    const auto video_duration = host.playback.player.duration();
    host.playback.duration.store(host.playback.video_master_clock && video_duration > 0.0
        ? video_duration
        : (audio_duration > 0.0 ? audio_duration : video_duration));
}



RECT fallback_stage_rect(HWND window) {
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        return info.rcMonitor;
    }
    return RECT{
        0, 0, static_cast<LONG>(kInitialWidth),
        static_cast<LONG>(kInitialHeight),
    };
}



RECT active_stage_rect(ShowHost& host) {
    auto rect = host.stage_rect_valid
        ? host.stage_rect
        : fallback_stage_rect(host.window);
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        rect = fallback_stage_rect(host.window);
    }
    return rect;
}
