#pragma once

#include "cast_nv12_surface_pool.h"
#include "cast_overlay_renderer.h"
#include "cast_pcm_resampler.h"
#include "cast_pipeline_diagnostics.h"
#include "cast_program_clock.h"
#include "cast_session_lifecycle.h"
#include "cast_stage_renderer.h"
#include "cast_video_frame_normalizer.h"
#include "d3d11_cast_capabilities.h"
#include "d3d11_nv12_overlay_compositor.h"
#include "mf_aac_encoder.h"
#include "mf_d3d11_h264_encoder.h"
#include "native_cast_backend.h"
#include "../audio/audio_player.h"
#include "../media/mf_d3d11_video_decoder.h"

#include "kirakara/show/config.hpp"
#include "kirakara/show/stage_overlay.hpp"
#include "kirakara/show/types.hpp"
#include "kirakara/show/win32/lyric_renderer.hpp"
#include "kirakara/show/win32/song_title_renderer.hpp"

#include <d3d11.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct CastProgramSnapshot {
    std::shared_ptr<AudioPlayer> audio;
    std::shared_ptr<const kirakara::show::PreparedDocument> document;
    std::shared_ptr<const kirakara::show::AppConfig> config;
    std::shared_ptr<const kirakara::show::StageOverlayState> stage_overlay;
    std::uint64_t stage_overlay_revision{};
    std::wstring video_source;
    bool media_loaded{};
    bool video_master_clock{};
    double audio_clock_offset_seconds{};
    std::uint64_t video_timeline_revision{};

    [[nodiscard]] bool valid() const noexcept;
};

struct CastProgramBindings {
    const std::atomic<int>* playback_state{};

    std::atomic<double>* published_position{};
    std::atomic<double>* published_duration{};
    std::atomic<bool>* published_buffering{};
    std::atomic<double>* buffering_position{};
    HWND notification_window{};

    [[nodiscard]] bool valid() const noexcept;
};

// Owns the complete Cast media/graphics/transport pipeline. Its worker is the
// sole per-frame producer and the sole owner of Cast COM/MF/D3D/MFT runtime
// calls; no Native Stage resource is owned here.
struct CastSession {
    CastProgramBindings program;
    // A frame atomically acquires one immutable publication and keeps it alive
    // until submission finishes. Host loads can therefore publish the next
    // program without waiting for decoder synchronization in the old frame.
    std::atomic<std::shared_ptr<const CastProgramSnapshot>> program_snapshot;
    kirakara::show::win32::LyricRenderer lyric_renderer;
    kirakara::show::win32::SongTitleRenderer title_renderer;
    CastStageRenderer stage_renderer;
    MfD3D11VideoDecoder video_decoder;
    CastVideoFrameNormalizer video_normalizer;
    CastOverlayRenderer overlay_renderer;
    D3D11Nv12OverlayCompositor nv12_overlay_compositor;
    CastNv12SurfacePool nv12_idle_pool;
    std::shared_ptr<const CastNormalizedVideoFrame> nv12_idle_frame;
    std::shared_ptr<const D3D11Nv12CompositedFrame> last_nv12_frame;
    D3D11CastBackend active_backend{D3D11CastBackend::compatibility_c};
    bool title_renderer_ready{};

    std::uint64_t frame_index{};
    std::chrono::steady_clock::time_point next_frame_due{};
    CastProgramClock program_clock;
    CastPipelineDiagnostics pipeline_diagnostics;
    std::mutex capability_mutex;
    D3D11CastCapabilityReport capability_report;
    bool capability_report_valid{};
    std::uint64_t last_decoded_frame_id{};
    std::uint64_t last_decoded_generation{};
    bool last_decoded_identity_valid{};
    CastSessionLifecycle lifecycle;

    // The Cast event loop is the sole producer of encoded video/audio frames.
    // During a synchronous host load it switches to keepalive mode, retaining
    // transport continuity without a second producer touching the encoders.
    // worker_mutex is the frame lock: the worker holds it for a whole frame,
    // so only the host command fence may wait on it. worker_queue_mutex guards
    // worker_commands alone and is never held across worker work, so posting a
    // command never waits for a frame in flight.
    std::mutex worker_mutex;
    std::mutex worker_queue_mutex;
    std::thread worker;
    HANDLE worker_wake_event{};
    HANDLE worker_stop_event{};
    HANDLE worker_timer{};
    HANDLE worker_ready_event{};
    std::atomic<bool> worker_startup_failed{false};
    std::deque<std::function<void()>> worker_commands;
    bool worker_accept_commands{};
    bool worker_render_enabled{};
    // Published by the host thread, observed by the worker before its next
    // frame; deliberately lock-free so a host load never waits for a frame.
    std::atomic<bool> load_keepalive_active{false};
    std::atomic<bool> worker_stop{true};

    NativeCastBackend backend;
    std::atomic<std::uint16_t> published_port{0};
    MfD3D11H264Encoder video_encoder;
    MfAacEncoder audio_encoder;
    CastPcmResampler audio_resampler;
    std::uint64_t audio_submitted_frames{};
    std::uint64_t audio_timeline_revision{};
    std::uint64_t audio_source_cursor{};
    double audio_program_frame{};
    bool audio_cursor_valid{};
    std::vector<std::int16_t> audio_source_scratch;
    std::vector<std::int16_t> audio_output_scratch;
    bool lyric_renderer_ready{};
};

double cast_monotonic_seconds(
    std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now()) noexcept;
CastVideoBufferAvailability cast_buffer_availability(
    MfD3D11VideoAvailability availability) noexcept;
CastVideoBufferSnapshot cast_buffer_snapshot(
    const MfD3D11VideoBufferStatus& status) noexcept;

kirakara::show::win32::StageCanvas active_cast_canvas(CastSession& cast);
void* active_cast_stage_texture(CastSession& cast);
bool configure_cast_nv12_pipeline(
    CastSession& cast,
    ID3D11Device* device,
    const kirakara::show::win32::StageCanvas& canvas,
    bool separate_encoder_texture);
bool cast_uses_compute_nv12(const CastSession& cast) noexcept;
void reset_cast_nv12_pipeline(CastSession& cast) noexcept;

void clear_cast_video_start_gate(CastSession& cast);
void reset_cast_audio_alignment(CastSession& cast);

void cast_load_keepalive_main(
    CastSession& cast, kirakara::show::win32::StageCanvas canvas);
bool start_cast_load_keepalive(
    CastSession& cast, const kirakara::show::win32::StageCanvas& canvas);
void stop_cast_load_keepalive(CastSession& cast);
bool begin_cast_load_keepalive(CastSession& cast);
void end_cast_load_keepalive(CastSession& cast, bool was_active);
void publish_cast_program_snapshot(
    CastSession& cast,
    std::shared_ptr<AudioPlayer> audio,
    const kirakara::show::PreparedDocument& document,
    const kirakara::show::AppConfig& config,
    const kirakara::show::StageOverlayState& stage_overlay,
    std::uint64_t stage_overlay_revision,
    const std::wstring& video_source,
    bool media_loaded,
    bool video_master_clock,
    double audio_clock_offset_seconds,
    std::uint64_t video_timeline_revision);
void publish_cast_overlay_snapshot(
    CastSession& cast,
    const kirakara::show::StageOverlayState& stage_overlay,
    std::uint64_t stage_overlay_revision);
void publish_cast_timeline_snapshot(
    CastSession& cast,
    double audio_clock_offset_seconds,
    std::uint64_t video_timeline_revision);
void refresh_cast_render_configuration(CastSession& cast);
void suspend_cast_video(CastSession& cast);
void post_cast_worker(CastSession& cast, std::function<void()> operation);

bool start_cast_pipeline(CastSession& cast, std::uint16_t port);
bool prepare_cast_pipeline(
    CastSession& cast, double takeover_position, bool prime_current_video);
void enable_cast_pipeline(CastSession& cast);
double stop_cast_pipeline(CastSession& cast);

void render_cast_frame(CastSession& cast);
