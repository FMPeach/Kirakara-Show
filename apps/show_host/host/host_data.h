#pragma once

// Prevent windows.h from pulling in the legacy winsock.h so we can
// safely include winsock2.h first.
#ifndef _WINSOCKAPI_
#include <winsock2.h>
#endif

#include "../audio/audio_player.h"
#include "../cast/cast_session.h"
#include "../media/media_engine_player.h"
#include "../stage/native_stage_session.h"
#include "host_utils.h"
#include "stage_output_lease.h"

#include "kirakara/show/config.hpp"
#include "kirakara/show/stage_overlay.hpp"
#include "kirakara/show/types.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

struct AudioProcessingSettings {
    int volume_percent{72};
    int key_semitones{};
    double clock_offset_seconds{};
};

struct LoadRequest {
    std::wstring video;
    std::wstring lyric;
    std::wstring vocal;
    std::wstring accompaniment;
    int clock_mode{};
    int transition_mode{};
};

struct CurrentMediaSource {
    std::wstring video;
    std::wstring lyric;
    std::wstring vocal;
    std::wstring accompaniment;
    int clock_mode{SHOW_CLOCK_AUDIO_MASTER};
    bool loaded{};

    void clear() {
        video.clear();
        lyric.clear();
        vocal.clear();
        accompaniment.clear();
        clock_mode = SHOW_CLOCK_AUDIO_MASTER;
        loaded = false;
    }

    void publish(const LoadRequest& request) {
        video = request.video;
        lyric = request.lyric;
        vocal = request.vocal;
        accompaniment = request.accompaniment;
        clock_mode = request.clock_mode;
        loaded = true;
    }
};

struct PreparedMediaPayload {
    LoadRequest request;
    std::uint64_t prepare_generation{};
    kirakara::show::PreparedDocument document;
    kirakara::show::AppConfig config{kirakara::show::default_app_config()};
    std::unique_ptr<AudioPlayer> audio;
    bool lyric_ready{};
    bool audio_ready{};
    int audio_track{SHOW_AUDIO_TRACK_VOCAL};
};

struct PreparedMediaState {
    std::mutex mutex;
    std::atomic<bool> alive{true};
    std::atomic<std::uint64_t> generation{0};
    std::unique_ptr<PreparedMediaPayload> ready;
    std::vector<std::thread> workers;
};

struct StageWindowRectRequest {
    int x{};
    int y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

struct StageOverlayRequest {
    kirakara::show::StageOverlayState state;
};

// Host control thread ownership: the creation handshake, the message-loop
// identity and the thread handle. Cast frame scheduling belongs to CastSession's
// worker and is not driven by this thread.
struct HostThreadState {
    std::thread worker;
    DWORD id{};
    std::mutex ready_mutex;
    std::condition_variable ready_cv;
    bool ready{};
    bool startup_failed{};
};

struct HostPlaybackRuntime {
    MediaEnginePlayer player;
    // Program audio is shared with immutable Cast publications. A switch can
    // replace this pointer immediately while an in-flight Cast frame safely
    // finishes with the previous player.
    std::shared_ptr<AudioPlayer> audio{std::make_shared<AudioPlayer>()};
    std::shared_ptr<PreparedMediaState> prepared_media{
        std::make_shared<PreparedMediaState>()};

    kirakara::show::PreparedDocument document;
    kirakara::show::AppConfig config{kirakara::show::default_app_config()};
    kirakara::show::StageOverlayState stage_overlay_state;
    std::atomic<std::uint64_t> stage_overlay_revision{};

    CurrentMediaSource current_media;
    std::wstring vocal_path;
    std::wstring accompaniment_path;
    AudioProcessingSettings audio_processing;
    int audio_track{SHOW_AUDIO_TRACK_VOCAL};
    std::atomic<std::uint64_t> playback_revision{0};
    // Changes only when the video source or timeline discontinuously changes.
    // Audio-only controls must not invalidate the decoded Cast texture queue.
    std::atomic<std::uint64_t> video_timeline_revision{0};
    double paused_position{};
    bool has_paused_position{};
    bool video_master_clock{};
    std::atomic<bool> video_buffering{false};
    std::atomic<double> video_buffering_position{0.0};
    std::chrono::steady_clock::time_point last_audio_sync{};

    std::atomic<int> state{SHOW_STATE_IDLE};
    std::atomic<double> position{0.0};
    std::atomic<double> duration{0.0};
};

struct OutputTransitionRuntime {
    bool native_video_suspended_for_cast{};
    // Cast closes MediaEngine entirely after taking over. Keeping this fact
    // separate from the old "paused for audio-master" flag lets stop-Cast
    // recreate local playback from current_media at the program-clock time.
    bool cast_media_engine_detached{};
    std::atomic<bool> media_engine_video_source_active{};
    std::atomic<bool> cast_video_source_active{};
    StageOutputLease stage_output_lease;

    // Seamless source changes hold the previous fully composed program until
    // the replacement is ready. Native and Cast outputs take over separately.
    bool native_transition_holding{};
    std::chrono::steady_clock::time_point transition_hold_started{};
    // One-shot guard so expire_stalled_program_transition logs its
    // "past takeover window" warning only once per transition hold cycle.
    bool native_transition_warned{};
};

static_assert(!std::is_copy_constructible_v<HostPlaybackRuntime>);
static_assert(!std::is_copy_constructible_v<NativeStageSession>);
static_assert(!std::is_copy_constructible_v<OutputTransitionRuntime>);
static_assert(!std::is_copy_constructible_v<CastSession>);

struct ShowHost {
    HostThreadState thread;

    HWND window{};
    // The Stage presentation surface window. It is sized to the Stage client
    // area and shown whenever a Stage sink (physical display or Flutter
    // texture) is active; the old name implied blanking, which it never did.
    HWND stage_surface_window{};
    HWND media_clock_window{};
    bool render_loop_active{};
    std::atomic<std::uint32_t> frame_width{kInitialWidth};
    std::atomic<std::uint32_t> frame_height{kInitialHeight};
    RECT stage_rect{0, 0, static_cast<LONG>(kInitialWidth),
        static_cast<LONG>(kInitialHeight)};
    bool stage_rect_valid{};

    HostPlaybackRuntime playback;
    NativeStageSession native_stage;
    OutputTransitionRuntime output_transition;
    CastSession cast;
};

    [[nodiscard]] inline bool has_physical_stage_output(
        const ShowHost& host) noexcept {
    return host.output_transition.stage_output_lease.is_held_by(
        StageOutputMode::physical_display);
}

    [[nodiscard]] inline bool has_ts_stage_output(
        const ShowHost& host) noexcept {
    return host.output_transition.stage_output_lease.is_held_by(
        StageOutputMode::ts_stream);
}
