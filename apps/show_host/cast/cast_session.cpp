// Cast session implementation. The Cast worker owns frame scheduling and the
// COM/MF/D3D pipeline; the host thread only coordinates shared output state.

#include "cast_session.h"

#include "aac_adts_packetizer.h"
#include "cast_av_sync.h"
#include "../host/host_commands.h"
#include "../host/host_utils.h"
#include "../show_host_api.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <future>
#include <optional>
#include <utility>

bool CastProgramSnapshot::valid() const noexcept {
    return audio && document && config && stage_overlay;
}

bool CastProgramBindings::valid() const noexcept {
    return playback_state && published_position && published_duration
        && published_buffering && buffering_position;
}

namespace {

struct CastCurrentMediaView {
    const std::wstring& video;
    bool loaded{};
};

struct CastAudioProcessingView {
    double clock_offset_seconds{};
};

struct CastPlaybackView {
    AudioPlayer& audio;
    const kirakara::show::PreparedDocument& document;
    const kirakara::show::AppConfig& config;
    const kirakara::show::StageOverlayState& stage_overlay_state;
    std::uint64_t stage_overlay_revision{};
    CastCurrentMediaView current_media;
    CastAudioProcessingView audio_processing;
    bool video_master_clock{};
    std::uint64_t video_timeline_revision{};
    const std::atomic<int>& state;
    std::atomic<double>& position;
    std::atomic<double>& duration;
    std::atomic<bool>& video_buffering;
    std::atomic<double>& video_buffering_position;
};

struct CastExecutionContext {
    std::shared_ptr<const CastProgramSnapshot> program;
    CastSession& cast;
    CastPlaybackView playback;
    HWND window{};
};

std::optional<CastExecutionContext> execution_context(CastSession& cast) {
    auto snapshot = cast.program_snapshot.load(std::memory_order_acquire);
    if (!snapshot || !snapshot->valid() || !cast.program.valid()) {
        return std::nullopt;
    }
    auto& bindings = cast.program;
    return CastExecutionContext{
        snapshot,
        cast,
        CastPlaybackView{
            *snapshot->audio,
            *snapshot->document,
            *snapshot->config,
            *snapshot->stage_overlay,
            snapshot->stage_overlay_revision,
            CastCurrentMediaView{
                snapshot->video_source, snapshot->media_loaded},
            CastAudioProcessingView{snapshot->audio_clock_offset_seconds},
            snapshot->video_master_clock,
            snapshot->video_timeline_revision,
            *bindings.playback_state,
            *bindings.published_position,
            *bindings.published_duration,
            *bindings.published_buffering,
            *bindings.buffering_position},
        bindings.notification_window};
}

}  // namespace

bool invoke_cast_worker(
    CastSession& cast,
    std::function<bool()> operation,
    std::chrono::milliseconds wait_timeout = std::chrono::milliseconds::zero());
void post_cast_worker(CastSession& cast, std::function<void()> operation);

kirakara::show::win32::StageCanvas active_cast_canvas(CastSession& cast) {
    return cast.stage_renderer.compositor().canvas();
}

void* active_cast_stage_texture(CastSession& cast) {
    return cast.stage_renderer.compositor().current_frame().overlay_texture;
}

static CastProgramClockUpdate update_cast_program_clock(
        CastExecutionContext& host,
        const MfD3D11VideoBufferStatus& decoder_status,
        bool has_video,
        std::chrono::steady_clock::time_point now) {
    CastProgramClockInput input;
    input.monotonic_seconds = cast_monotonic_seconds(now);
    input.playback_requested =
        host.playback.state.load() == SHOW_STATE_PLAYING;
    input.has_video = has_video;
    input.has_audio_stream = host.playback.audio.is_open();
    input.has_audio_clock =
        input.has_audio_stream && !host.playback.video_master_clock;
    if (input.has_audio_stream) {
        input.audio_position_seconds = std::max(0.0,
            host.playback.audio.position()
                + host.playback.audio_processing.clock_offset_seconds);
        input.end_on_audio_eof = input.has_audio_clock;
        input.audio_end_of_stream = host.playback.audio.has_ended();
        input.audio_duration_seconds = host.playback.audio.duration();
    }
    input.video_buffer = cast_buffer_snapshot(decoder_status);

    const auto previous_state = host.cast.program_clock.state();
    auto update = host.cast.program_clock.update(input);
    if (input.has_audio_stream) {
        if (update.request_audio_pause) {
            static_cast<void>(host.playback.audio.pause());
        }
        if (update.request_audio_seek) {
            // The clock is expressed in the video/lyric program timeline;
            // audio clock offset is applied while sampling, so remove it when
            // seeking the underlying audio stream after an underflow.
            static_cast<void>(host.playback.audio.seek(std::max(0.0,
                update.audio_seek_position_seconds
                    - host.playback.audio_processing.clock_offset_seconds)));
        }
        if (update.request_audio_play) {
            static_cast<void>(host.playback.audio.play());
        }
    }

    const bool waiting_for_video = has_video
        && (update.state == CastProgramClockState::priming
            || update.state == CastProgramClockState::underflow);
    host.playback.video_buffering.store(waiting_for_video);
    if (waiting_for_video) {
        host.playback.video_buffering_position.store(
            update.position_seconds, std::memory_order_relaxed);
    }
    host.playback.position.store(update.position_seconds);

    const auto audio_duration = host.playback.audio.duration();
    const auto video_duration = decoder_status.source_duration_seconds;
    if (host.playback.video_master_clock && video_duration > 0.0) {
        host.playback.duration.store(video_duration);
    } else if (audio_duration > 0.0) {
        host.playback.duration.store(audio_duration);
    } else if (video_duration > 0.0) {
        host.playback.duration.store(video_duration);
    }

    if (update.state == CastProgramClockState::fatal_error
            && previous_state != CastProgramClockState::fatal_error) {
        log_hresult("cast program clock: decoder fatal error",
            static_cast<HRESULT>(decoder_status.hresult));
        const auto revision = host.playback.video_timeline_revision;
        if (host.cast.lifecycle.should_notify_fatal(revision)) {
            // Window/Native state remains owned by the host thread. The Cast
            // worker only publishes this low-frequency terminal event.
            PostMessageW(host.window, kCastFatalErrorMessage, 0,
                static_cast<LPARAM>(revision));
        }
    }
    return update;
}

[[nodiscard]] bool configure_cast_nv12_pipeline(
        CastSession& cast,
        ID3D11Device* device,
        const kirakara::show::win32::StageCanvas& canvas,
        bool separate_encoder_texture) {
    reset_cast_nv12_pipeline(cast);
    if (!device || canvas.width == 0 || canvas.height == 0
            || (canvas.width & 1U) != 0 || (canvas.height & 1U) != 0) {
        return false;
    }

    CastVideoFrameNormalizerConfig normalizer_config;
    normalizer_config.width = canvas.width;
    normalizer_config.height = canvas.height;
    normalizer_config.frame_rate_num = canvas.frame_rate_num;
    normalizer_config.frame_rate_den = canvas.frame_rate_den;
    normalizer_config.surface_count = 4;
    normalizer_config.create_plane_srvs = true;
    normalizer_config.allow_direct_passthrough = true;
    if (!cast.video_normalizer.configure(
            device, normalizer_config, &cast.pipeline_diagnostics)) {
        reset_cast_nv12_pipeline(cast);
        return false;
    }
    if (!cast.overlay_renderer.configure(
            device, canvas.width, canvas.height,
            &cast.pipeline_diagnostics)) {
        reset_cast_nv12_pipeline(cast);
        return false;
    }

    D3D11Nv12OverlayCompositorConfig compositor_config;
    compositor_config.width = canvas.width;
    compositor_config.height = canvas.height;
    compositor_config.surface_count = 8;
    compositor_config.separate_encoder_texture = separate_encoder_texture;
    compositor_config.encoder_ready_empty_output = true;
    if (!cast.nv12_overlay_compositor.configure(
            device, compositor_config, &cast.pipeline_diagnostics)
            || !cast.nv12_overlay_compositor.prepare_overlay_texture(
                cast.overlay_renderer.frame().texture)) {
        reset_cast_nv12_pipeline(cast);
        return false;
    }

    CastNv12SurfacePoolConfig idle_config;
    idle_config.width = canvas.width;
    idle_config.height = canvas.height;
    idle_config.capacity = 1;
    idle_config.create_plane_srvs = true;
    idle_config.create_plane_uavs = false;
    idle_config.video_processor_output = false;
    idle_config.encoder_compatible = false;
    if (!cast.nv12_idle_pool.configure(
            device, idle_config, &cast.pipeline_diagnostics)) {
        reset_cast_nv12_pipeline(cast);
        return false;
    }
    auto idle_lease = cast.nv12_idle_pool.try_acquire();
    auto* idle_texture = static_cast<ID3D11Texture2D*>(
        idle_lease.compose_texture());
    if (!idle_lease || !idle_texture) {
        reset_cast_nv12_pipeline(cast);
        return false;
    }
    std::vector<std::uint8_t> black_nv12(
        static_cast<std::size_t>(canvas.width) * canvas.height * 3U / 2U,
        16);
    std::fill(
        black_nv12.begin()
            + static_cast<std::size_t>(canvas.width) * canvas.height,
        black_nv12.end(), 128);
    ID3D11DeviceContext* context{};
    device->GetImmediateContext(&context);
    if (!context) {
        reset_cast_nv12_pipeline(cast);
        return false;
    }
    context->UpdateSubresource(idle_texture, 0, nullptr,
        black_nv12.data(), canvas.width,
        static_cast<UINT>(black_nv12.size()));
    context->Release();
    if (FAILED(device->GetDeviceRemovedReason())) {
        reset_cast_nv12_pipeline(cast);
        return false;
    }

    MfD3D11VideoFormat idle_format;
    idle_format.pixel_format = MfD3D11VideoPixelFormat::nv12;
    idle_format.width = canvas.width;
    idle_format.height = canvas.height;
    idle_format.pixel_aspect_ratio_num = 1;
    idle_format.pixel_aspect_ratio_den = 1;
    idle_format.frame_rate_num = canvas.frame_rate_num;
    idle_format.frame_rate_den = canvas.frame_rate_den;
    try {
        cast.nv12_idle_frame =
            std::make_shared<CastNormalizedVideoFrame>(
                std::move(idle_lease), idle_format,
                0.0, 0.0, 0, 0);
    } catch (...) {
        reset_cast_nv12_pipeline(cast);
        return false;
    }
    return true;
}

// The Cast program is primed by the frame loop itself: render_cast_frame()
// synchronizes the decoder against the published program on every frame, so no
// host path may block on a priming round trip.
void clear_cast_video_start_gate(CastSession& cast) {
    cast.lifecycle.clear_start_gate();
}

static void release_cast_video_start_gate(CastExecutionContext& host) {
    if (!host.cast.lifecycle.snapshot().start_gate_pending) return;
    clear_cast_video_start_gate(host.cast);
    // Buffer watermarks have already released CastProgramClock on this frame.
    // This flag only commits visual takeover of the new program.
    host.playback.position.store(host.cast.program_clock.position_seconds());
}

void reset_cast_audio_alignment(CastSession& cast) {
    cast.audio_resampler.reset();
    cast.audio_timeline_revision = 0;
    cast.audio_source_cursor = 0;
    cast.audio_program_frame = 0.0;
    cast.audio_cursor_valid = false;
}

void shutdown_cast_pipeline_resources(CastSession& cast);

void shutdown_cast_renderers(CastSession& cast) {
    if (cast.title_renderer_ready) {
        cast.title_renderer.shutdown();
        cast.title_renderer_ready = false;
    }
    if (cast.lyric_renderer_ready) {
        cast.lyric_renderer.shutdown();
        cast.lyric_renderer_ready = false;
    }
}

void cast_load_keepalive_main(
        CastSession& cast,
        kirakara::show::win32::StageCanvas) {
    const auto com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto mf_result = MFStartup(MF_VERSION);
    cast.worker_startup_failed.store(
        FAILED(com_result) || FAILED(mf_result), std::memory_order_release);
    SetEvent(cast.worker_ready_event);
    if (cast.worker_startup_failed.load(std::memory_order_acquire)) {
        if (SUCCEEDED(mf_result)) MFShutdown();
        if (SUCCEEDED(com_result)) CoUninitialize();
        return;
    }
    const auto previous_priority = GetThreadPriority(GetCurrentThread());
    static_cast<void>(SetThreadPriority(
        GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL));

    std::vector<std::int16_t> silence;
    auto next_frame_due = std::chrono::steady_clock::now();

    for (;;) {
        bool render_enabled{};
        std::function<void()> command;
        {
            // The queue lock is never held across frame production, so a host
            // post always lands without waiting for the current frame.
            std::lock_guard lock(cast.worker_queue_mutex);
            if (cast.worker_stop.load(std::memory_order_acquire)) break;
            if (!cast.worker_commands.empty()) {
                command = std::move(cast.worker_commands.front());
                cast.worker_commands.pop_front();
            }
        }
        if (command) {
            command();
            continue;
        }
        {
            std::lock_guard lock(cast.worker_mutex);
            if (cast.worker_stop.load(std::memory_order_acquire)) break;
            render_enabled = cast.worker_render_enabled;
            if (cast.next_frame_due.time_since_epoch().count() != 0) {
                next_frame_due = cast.next_frame_due;
            }
        }

        if (!render_enabled) {
            const HANDLE handles[]{
                cast.worker_stop_event,
                cast.worker_wake_event,
            };
            if (WaitForMultipleObjects(2, handles, FALSE, INFINITE)
                    == WAIT_OBJECT_0) {
                break;
            }
            continue;
        }

        const auto canvas = active_cast_canvas(cast);
        if (canvas.width == 0 || canvas.height == 0
                || canvas.frame_rate_num == 0
                || canvas.frame_rate_den == 0) {
            const HANDLE handles[]{
                cast.worker_stop_event,
                cast.worker_wake_event,
            };
            if (WaitForMultipleObjects(2, handles, FALSE, INFINITE)
                    == WAIT_OBJECT_0) {
                break;
            }
            continue;
        }
        const auto frame_interval = std::chrono::duration_cast<
            std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(
                    static_cast<double>(canvas.frame_rate_den)
                        / static_cast<double>(canvas.frame_rate_num)));
        const auto frame_duration_100ns = static_cast<std::int64_t>(
            10000000ULL * canvas.frame_rate_den / canvas.frame_rate_num);

        const auto now = std::chrono::steady_clock::now();
        if (next_frame_due.time_since_epoch().count() == 0
                || next_frame_due < now
                || next_frame_due > now + frame_interval * 2) {
            next_frame_due = now;
        }
        const auto remaining = next_frame_due - now;
        const auto ticks_100ns = std::max<std::int64_t>(
            1,
            std::chrono::duration_cast<
                std::chrono::duration<std::int64_t,
                    std::ratio<1, 10000000>>>(remaining).count());
        LARGE_INTEGER due{};
        due.QuadPart = -ticks_100ns;
        if (!SetWaitableTimer(
                cast.worker_timer, &due, 0, nullptr, nullptr, FALSE)) {
            SetEvent(cast.worker_wake_event);
        }
        const HANDLE handles[]{
            cast.worker_stop_event,
            cast.worker_wake_event,
            cast.worker_timer,
        };
        const auto wait = WaitForMultipleObjects(3, handles, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_OBJECT_0 + 1) continue;
        if (wait != WAIT_OBJECT_0 + 2) continue;

        std::lock_guard lock(cast.worker_mutex);
        if (cast.worker_stop.load(std::memory_order_acquire)) break;
        if (!cast.worker_render_enabled) continue;

        if (!cast.load_keepalive_active.load(std::memory_order_acquire)) {
            // The event-loop thread is the only caller of the full Cast tick.
            // render_cast_frame owns the live-program deadline calculation.
            render_cast_frame(cast);
            next_frame_due = cast.next_frame_due;
            continue;
        }

        const auto submitted_frame = cast.frame_index;
        const auto frame_time_100ns = static_cast<std::int64_t>(
            submitted_frame) * frame_duration_100ns;
        if (cast_uses_compute_nv12(cast)) {
            const auto frame = cast.last_nv12_frame;
            if (frame && frame->texture) {
                static_cast<void>(cast.video_encoder.submit_nv12_texture(
                    frame->texture,
                    frame->retention,
                    frame_time_100ns,
                    frame_duration_100ns));
            }
        } else if (auto* texture = active_cast_stage_texture(cast)) {
            static_cast<void>(cast.video_encoder.submit_repeated_texture(
                texture,
                frame_time_100ns,
                frame_duration_100ns));
        }

        const auto target_audio_frames = static_cast<std::uint64_t>(
            (submitted_frame + 1ULL)
                * static_cast<std::uint64_t>(
                    CastPcmResampler::kOutputSampleRate)
                * canvas.frame_rate_den / canvas.frame_rate_num);
        if (target_audio_frames > cast.audio_submitted_frames) {
            const auto output_frames = static_cast<std::size_t>(
                target_audio_frames - cast.audio_submitted_frames);
            const auto output_samples = output_frames
                * CastPcmResampler::kOutputChannels;
            if (silence.size() != output_samples) {
                silence.assign(output_samples, std::int16_t{});
            }
            static_cast<void>(cast.audio_encoder.submit_pcm(
                silence.data(),
                output_frames,
                cast.audio_submitted_frames));
            cast.audio_submitted_frames = target_audio_frames;
        }
        ++cast.frame_index;

        next_frame_due += frame_interval;
        const auto completed = std::chrono::steady_clock::now();
        if (next_frame_due <= completed) {
            const auto missed =
                (completed - next_frame_due) / frame_interval + 1;
            next_frame_due += frame_interval * missed;
            cast.frame_index += static_cast<std::uint64_t>(missed);
            cast.pipeline_diagnostics.increment(
                CastPipelineCounter::missed_output_slots,
                static_cast<std::uint64_t>(missed));
        }
        cast.next_frame_due = next_frame_due;
    }

    // Every MF/MFT/D3D object used by Cast is stopped while COM and Media
    // Foundation are still active on the thread that created and drove it.
    shutdown_cast_pipeline_resources(cast);
    shutdown_cast_renderers(cast);
    if (previous_priority != THREAD_PRIORITY_ERROR_RETURN) {
        static_cast<void>(SetThreadPriority(GetCurrentThread(), previous_priority));
    }
    MFShutdown();
    CoUninitialize();
}

static bool enqueue_cast_worker(
        CastSession& cast, std::function<void()> command) {
    if (!command || !cast.worker.joinable()) return false;
    {
        // Only the queue lock: never waits for a frame in flight.
        std::lock_guard lock(cast.worker_queue_mutex);
        if (!cast.worker_accept_commands
                || cast.worker_stop.load(std::memory_order_acquire)) {
            return false;
        }
        cast.worker_commands.emplace_back(std::move(command));
    }
    SetEvent(cast.worker_wake_event);
    return true;
}

void post_cast_worker(CastSession& cast, std::function<void()> operation) {
    static_cast<void>(enqueue_cast_worker(cast, std::move(operation)));
}

bool invoke_cast_worker(
        CastSession& cast,
        std::function<bool()> operation,
        std::chrono::milliseconds wait_timeout) {
    if (!operation || !cast.worker.joinable()) return false;
    auto completion = std::make_shared<std::promise<bool>>();
    auto result = completion->get_future();
    if (!enqueue_cast_worker(
            cast,
            [operation = std::move(operation), completion]() mutable {
                try {
                    completion->set_value(operation());
                } catch (...) {
                    completion->set_exception(std::current_exception());
                }
            })) {
        return false;
    }
    if (wait_timeout.count() <= 0) {
        try {
            return result.get();
        } catch (...) {
            return false;
        }
    }
    if (result.wait_for(wait_timeout) != std::future_status::ready) {
        // Bounded wait: the command may still execute later, so callers that
        // pass a timeout must only capture state that outlives this call.
        return false;
    }
    try {
        return result.get();
    } catch (...) {
        return false;
    }
}

bool start_cast_load_keepalive(
        CastSession& cast,
        const kirakara::show::win32::StageCanvas& canvas) {
    if (!cast.program.valid()) return false;
    std::unique_lock lock(cast.worker_mutex);
    if (cast.worker.joinable()) return true;
    cast.worker_wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    cast.worker_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    cast.worker_ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    cast.worker_timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    if (!cast.worker_timer) {
        cast.worker_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    if (!cast.worker_wake_event || !cast.worker_stop_event
            || !cast.worker_ready_event || !cast.worker_timer) {
        if (cast.worker_timer) CloseHandle(cast.worker_timer);
        if (cast.worker_ready_event) CloseHandle(cast.worker_ready_event);
        if (cast.worker_stop_event) CloseHandle(cast.worker_stop_event);
        if (cast.worker_wake_event) CloseHandle(cast.worker_wake_event);
        cast.worker_timer = nullptr;
        cast.worker_ready_event = nullptr;
        cast.worker_stop_event = nullptr;
        cast.worker_wake_event = nullptr;
        return false;
    }
    // No worker exists yet, so this reset needs no lock.
    cast.load_keepalive_active.store(false, std::memory_order_relaxed);
    cast.worker_render_enabled = false;
    cast.worker_stop.store(false, std::memory_order_relaxed);
    {
        std::lock_guard lock(cast.worker_queue_mutex);
        cast.worker_commands.clear();
        cast.worker_accept_commands = true;
    }
    cast.worker_startup_failed.store(false, std::memory_order_relaxed);
    try {
        cast.worker = std::thread(
            [&cast, canvas] { cast_load_keepalive_main(cast, canvas); });
    } catch (...) {
        cast.worker_stop = true;
        CloseHandle(cast.worker_timer);
        CloseHandle(cast.worker_ready_event);
        CloseHandle(cast.worker_stop_event);
        CloseHandle(cast.worker_wake_event);
        cast.worker_timer = nullptr;
        cast.worker_ready_event = nullptr;
        cast.worker_stop_event = nullptr;
        cast.worker_wake_event = nullptr;
        return false;
    }
    const auto ready = WaitForSingleObject(cast.worker_ready_event, 5000);
    if (ready != WAIT_OBJECT_0
            || cast.worker_startup_failed.load(std::memory_order_acquire)) {
        cast.worker_stop = true;
        SetEvent(cast.worker_stop_event);
        SetEvent(cast.worker_wake_event);
        lock.unlock();
        if (cast.worker.joinable()) cast.worker.join();
        lock.lock();
        CloseHandle(cast.worker_timer);
        CloseHandle(cast.worker_ready_event);
        CloseHandle(cast.worker_stop_event);
        CloseHandle(cast.worker_wake_event);
        cast.worker_timer = nullptr;
        cast.worker_ready_event = nullptr;
        cast.worker_stop_event = nullptr;
        cast.worker_wake_event = nullptr;
        return false;
    }
    return true;
}

void stop_cast_load_keepalive(CastSession& cast) {
    {
        std::lock_guard lock(cast.worker_mutex);
        cast.load_keepalive_active.store(false, std::memory_order_release);
        cast.worker_render_enabled = false;
        cast.worker_stop.store(true, std::memory_order_release);
    }
    {
        // Never hold the frame lock and the queue lock at the same time.
        std::lock_guard lock(cast.worker_queue_mutex);
        cast.worker_accept_commands = false;
        // Destroying a queued invoke command also destroys its promise, so a
        // synchronous waiter observes broken_promise instead of hanging.
        cast.worker_commands.clear();
    }
    if (cast.worker_stop_event) SetEvent(cast.worker_stop_event);
    if (cast.worker_wake_event) SetEvent(cast.worker_wake_event);
    if (cast.worker.joinable()) {
        cast.worker.join();
    }
    if (cast.worker_timer) CloseHandle(cast.worker_timer);
    if (cast.worker_ready_event) CloseHandle(cast.worker_ready_event);
    if (cast.worker_stop_event) CloseHandle(cast.worker_stop_event);
    if (cast.worker_wake_event) CloseHandle(cast.worker_wake_event);
    cast.worker_timer = nullptr;
    cast.worker_ready_event = nullptr;
    cast.worker_stop_event = nullptr;
    cast.worker_wake_event = nullptr;
    cast.worker_startup_failed.store(false, std::memory_order_relaxed);
}

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
        std::uint64_t video_timeline_revision) {
    auto next = std::make_shared<CastProgramSnapshot>();
    next->audio = std::move(audio);
    next->document =
        std::make_shared<kirakara::show::PreparedDocument>(document);
    next->config = std::make_shared<kirakara::show::AppConfig>(config);
    next->stage_overlay =
        std::make_shared<kirakara::show::StageOverlayState>(stage_overlay);
    next->stage_overlay_revision = stage_overlay_revision;
    next->video_source = video_source;
    next->media_loaded = media_loaded;
    next->video_master_clock = video_master_clock;
    next->audio_clock_offset_seconds = audio_clock_offset_seconds;
    next->video_timeline_revision = video_timeline_revision;
    cast.program_snapshot.store(std::move(next), std::memory_order_release);
}

void publish_cast_overlay_snapshot(
        CastSession& cast,
        const kirakara::show::StageOverlayState& stage_overlay,
        std::uint64_t stage_overlay_revision) {
    auto current = cast.program_snapshot.load(std::memory_order_acquire);
    if (!current) return;
    auto next = std::make_shared<CastProgramSnapshot>(*current);
    next->stage_overlay =
        std::make_shared<kirakara::show::StageOverlayState>(stage_overlay);
    next->stage_overlay_revision = stage_overlay_revision;
    cast.program_snapshot.store(std::move(next), std::memory_order_release);
}

void publish_cast_timeline_snapshot(
        CastSession& cast,
        double audio_clock_offset_seconds,
        std::uint64_t video_timeline_revision) {
    auto current = cast.program_snapshot.load(std::memory_order_acquire);
    if (!current) return;
    auto next = std::make_shared<CastProgramSnapshot>(*current);
    next->audio_clock_offset_seconds = audio_clock_offset_seconds;
    next->video_timeline_revision = video_timeline_revision;
    cast.program_snapshot.store(std::move(next), std::memory_order_release);
}

bool begin_cast_load_keepalive(CastSession& cast) {
    if (!cast.worker.joinable()
            || cast.worker_stop.load(std::memory_order_acquire)) {
        return false;
    }
    // Publishing the flag is enough: the worker reads it before producing its
    // next frame. Waiting for worker_mutex here would stall the calling host
    // thread behind a whole Cast frame - and behind a decoder synchronize()
    // still opening a slow progressive source.
    cast.load_keepalive_active.store(true, std::memory_order_release);
    SetEvent(cast.worker_wake_event);
    return true;
}

void end_cast_load_keepalive(CastSession& cast, bool was_active) {
    if (!was_active) return;
    // The frame deadline kept advancing during keepalive, so the worker is due
    // immediately once the flag is clear.
    cast.load_keepalive_active.store(false, std::memory_order_release);
    SetEvent(cast.worker_wake_event);
}

void refresh_cast_render_configuration(CastSession& cast) {
    if (!cast.worker.joinable() || !cast.program.valid()) return;
    // Posted, not awaited: the host load path must never wait for a Cast frame.
    post_cast_worker(cast, [&cast] {
        const auto program =
            cast.program_snapshot.load(std::memory_order_acquire);
        if (!program || !program->valid()) return;
        if (!cast.lyric_renderer_ready) {
            cast.lyric_renderer_ready = cast.lyric_renderer.initialize();
            if (!cast.lyric_renderer_ready) return;
        }
        cast.lyric_renderer.set_font_families(
            program->config->font_families);
        if (program->config->song_title.enabled
                && !cast.title_renderer_ready) {
            cast.title_renderer_ready = cast.title_renderer.initialize();
            if (!cast.title_renderer_ready) {
                log_line(
                    "cast title renderer unavailable; continuing without title");
            }
        }
    });
}

void suspend_cast_video(CastSession& cast) {
    if (!cast.worker.joinable()) return;
    // Best effort with a bounded wait: the host clears the published source
    // around this call, so the worker idles on its own when the wait expires.
    static_cast<void>(invoke_cast_worker(
        cast,
        [&cast] {
            cast.video_decoder.suspend();
            return true;
        },
        std::chrono::milliseconds(250)));
}

void shutdown_cast_pipeline_resources(CastSession& cast) {
    cast.audio_encoder.stop();
    cast.video_encoder.stop();
    cast.backend.stop();
    cast.published_port.store(0, std::memory_order_release);
    cast.video_decoder.stop();
    reset_cast_nv12_pipeline(cast);
    {
        std::lock_guard lock(cast.capability_mutex);
        cast.capability_report.backend_active = false;
    }
}

bool start_cast_pipeline(CastSession& cast, std::uint16_t port) {
    cast.lifecycle.begin_start();
    if (!cast.program.valid() || !start_cast_load_keepalive(cast, {})) {
        cast.lifecycle.stop();
        return false;
    }
    const bool pipeline_started = invoke_cast_worker(cast, [&]() -> bool {
        auto context = execution_context(cast);
        if (!context) return false;
        auto& host = *context;
        if (!host.cast.lyric_renderer_ready) {
            host.cast.lyric_renderer_ready =
                host.cast.lyric_renderer.initialize();
        }
        if (!host.cast.lyric_renderer_ready) return false;
        host.cast.lyric_renderer.set_font_families(
            host.playback.config.font_families);
        host.cast.pipeline_diagnostics.reset();
        host.cast.published_port.store(0, std::memory_order_release);
        {
            std::lock_guard lock(host.cast.capability_mutex);
            host.cast.capability_report = {};
            host.cast.capability_report_valid = false;
        }
        host.cast.backend.set_diagnostics(&host.cast.pipeline_diagnostics);
        if (host.playback.config.song_title.enabled
                && !host.cast.title_renderer_ready) {
            host.cast.title_renderer_ready =
                host.cast.title_renderer.initialize();
            if (!host.cast.title_renderer_ready) {
                log_line(
                    "cast title renderer unavailable; continuing without title");
            }
        }
    struct CastProfile {
        kirakara::show::win32::StageCanvas canvas;
        std::uint32_t bitrate;
        MfH264EncoderPreference encoder_preference;
    };
        const std::array profiles{
        CastProfile{
            {1920, 1080, 60, 1},
            6000000,
            MfH264EncoderPreference::hardware_only},
        CastProfile{
            {1920, 1080, 60, 1},
            6000000,
            MfH264EncoderPreference::hardware_readback_only},
        CastProfile{
            {1920, 1080, 30, 1},
            4500000,
            MfH264EncoderPreference::hardware_only},
        CastProfile{
            {1920, 1080, 30, 1},
            4500000,
            MfH264EncoderPreference::hardware_readback_only},
        CastProfile{
            {1920, 1080, 30, 1},
            4000000,
            MfH264EncoderPreference::software_only},
    };

        bool capability_probed{};
        D3D11CastCapabilityReport capability_report;

        for (const auto& profile : profiles) {
        if (!host.cast.stage_renderer.configure(profile.canvas)) continue;
        auto* cast_device = host.cast.stage_renderer.native_d3d_device();
        if (!cast_device) {
            continue;
        }
        if (!capability_probed) {
            D3D11CastCapabilityProbeOptions probe_options;
            probe_options.width = profile.canvas.width;
            probe_options.height = profile.canvas.height;
            probe_options.frame_rate_num = profile.canvas.frame_rate_num;
            probe_options.frame_rate_den = profile.canvas.frame_rate_den;
            // Do not synchronously open/read the current media source here.
            // show_host_start_cast_stream is an ABI-compatible synchronous
            // call used directly from Flutter's UI isolate, and an external or
            // still-growing source can block MFCreateSourceReader/ReadSample
            // indefinitely. The sole production decoder negotiates the real
            // source asynchronously after Cast has started.
            probe_options.decoder_source = nullptr;
            // Keep startup negotiation inside cheap D3D11 resource creation.
            // The production encoder below validates its own first NV12
            // sample; activating a disposable hardware MFT here can spend
            // tens of seconds inside an Intel driver before activating the
            // same MFT a second time for the real stream.
            probe_options.probe_hardware_encoder = false;
            capability_report =
                D3D11CastCapabilityProbe::run(cast_device, probe_options);
            capability_probed = true;
        }

        const auto provisional_report =
            select_d3d11_cast_preflight(capability_report);
        const bool use_compute_nv12 =
            profile.encoder_preference
                == MfH264EncoderPreference::hardware_only
            && provisional_report.backend
                == D3D11CastBackend::compute_nv12_a;
        MfD3D11VideoDecoderConfig decoder_config;
        decoder_config.output_format = use_compute_nv12
            ? MfD3D11VideoPixelFormat::nv12
            : MfD3D11VideoPixelFormat::bgra8;
        decoder_config.max_queued_frames = kCastDecodedFrameCapacity;
        decoder_config.max_queued_bytes = kCastDecodedByteCapacity;
        decoder_config.decode_ahead_seconds = kCastDecodeAheadSeconds;
        decoder_config.observe_local_source_growth = true;
        host.cast.video_decoder.stop();
        if (!host.cast.video_decoder.start(
                cast_device, decoder_config,
                &host.cast.pipeline_diagnostics)) {
            continue;
        }
        if (use_compute_nv12) {
            // No media is open yet, but the sole production decoder endpoint
            // has successfully initialized in native-NV12 mode. Per-source
            // negotiation happens when synchronize() opens the first program;
            // a later failure remains on the existing frame and never switches
            // backend in-flight. Phase 5 exposes its fatal/source-starved state.
            capability_report.native_nv12_source = {
                true, true, static_cast<std::int32_t>(S_OK)};
        }
        if (use_compute_nv12
                && !configure_cast_nv12_pipeline(
                    host.cast, static_cast<ID3D11Device*>(cast_device),
                    profile.canvas,
                    provisional_report.requires_encoder_copy)) {
            log_line("cast stream: compute NV12 pipeline initialization failed");
            host.cast.video_decoder.stop();
            reset_cast_nv12_pipeline(host.cast);
            return false;
        }
        if (!use_compute_nv12) reset_cast_nv12_pipeline(host.cast);

        // Compose one real encoder-ready black frame before MFT activation.
        // Passing this frame into start() lets the production encoder prove
        // ProcessInput compatibility without a disposable probe encoder.
        if (use_compute_nv12 && host.cast.nv12_idle_frame) {
            const auto& idle = *host.cast.nv12_idle_frame;
            host.cast.last_nv12_frame =
                host.cast.nv12_overlay_compositor.compose(
                    D3D11Nv12OverlayInput{
                        idle.texture(),
                        idle.y_srv(),
                        idle.uv_srv(),
                        idle.retention_token(),
                        idle.frame_id(),
                        idle.generation()},
                    host.cast.overlay_renderer.frame(),
                    0);
            if (!host.cast.last_nv12_frame
                    || !host.cast.last_nv12_frame->texture
                    || !host.cast.last_nv12_frame->retention) {
                host.cast.video_decoder.stop();
                reset_cast_nv12_pipeline(host.cast);
                continue;
            }
        }

        MfD3D11H264EncoderConfig encoder_config{};
        encoder_config.width = profile.canvas.width;
        encoder_config.height = profile.canvas.height;
        encoder_config.frame_rate_num = profile.canvas.frame_rate_num;
        encoder_config.frame_rate_den = profile.canvas.frame_rate_den;
        encoder_config.bitrate = profile.bitrate;
        encoder_config.preference = profile.encoder_preference;
        encoder_config.input_format = use_compute_nv12
            ? MfH264EncoderInputFormat::nv12
            : MfH264EncoderInputFormat::bgra8;
        encoder_config.gop_size_frames = std::max<std::uint32_t>(
            1,
            profile.canvas.frame_rate_num
                / (2U * profile.canvas.frame_rate_den));
        host.cast.backend.reset_stream();
        const auto encoder_started = host.cast.video_encoder.start(
            cast_device,
            encoder_config,
            [&cast](const std::uint8_t* data,
                    std::size_t size,
                    std::int64_t pts90k,
                    bool keyframe) {
                cast.backend.submit_h264_access_unit(
                    data, size, pts90k, keyframe);
            },
            &host.cast.pipeline_diagnostics,
            use_compute_nv12
                ? host.cast.last_nv12_frame->texture : nullptr,
            use_compute_nv12
                ? host.cast.last_nv12_frame->retention
                : std::shared_ptr<void>{});
        if (!encoder_started) {
            if (use_compute_nv12) {
                capability_report.dxgi_h264_encoder = {
                    true, false, static_cast<std::int32_t>(E_FAIL)};
                capability_report.reason =
                    D3D11CastBackendReason::dxgi_encoder_unavailable;
            }
            host.cast.video_decoder.stop();
            reset_cast_nv12_pipeline(host.cast);
            continue;
        }
        if (use_compute_nv12) {
            capability_report.reason = provisional_report.reason;
            capability_report.dxgi_h264_encoder = {
                true, true, static_cast<std::int32_t>(S_OK)};
            capability_report.tracked_sample_release = {
                true, true, static_cast<std::int32_t>(S_OK)};
            capability_report.encoder_retains_samples_async =
                host.cast.video_encoder.startup_sample_retained_async();
            capability_report.requires_encoder_copy =
                provisional_report.requires_encoder_copy;
            if (provisional_report.requires_encoder_copy) {
                capability_report.compose_to_encoder_copy = {
                    true, true, static_cast<std::int32_t>(S_OK)};
            } else {
                capability_report.combined_uav_encoder_input = {
                    true, true, static_cast<std::int32_t>(S_OK)};
            }
        }
        if (!use_compute_nv12
                && !host.cast.video_encoder.prepare_input_texture(
                    active_cast_stage_texture(host.cast))) {
            host.cast.video_encoder.stop();
            host.cast.video_decoder.stop();
            continue;
        }

        const auto audio_encoder_started = host.cast.audio_encoder.start(
            MfAacEncoderConfig{},
            [&cast, adts = std::vector<std::uint8_t>{}](
                    const std::uint8_t* data,
                    std::size_t size,
                    std::int64_t pts90k) mutable {
                adts.clear();
                if (AacAdtsPacketizer::append_frame(
                        AacAdtsConfig{
                            CastPcmResampler::kOutputSampleRate,
                            CastPcmResampler::kOutputChannels,
                            2},
                        data,
                        size,
                        adts)) {
                    cast.backend.submit_aac_adts_frame(
                        adts.data(), adts.size(), pts90k);
                }
            });
        if (!audio_encoder_started) {
            host.cast.video_encoder.stop();
            host.cast.video_decoder.stop();
            reset_cast_nv12_pipeline(host.cast);
            break;
        }

        if (!host.cast.backend.start_http(port)) {
            host.cast.audio_encoder.stop();
            host.cast.video_encoder.stop();
            host.cast.video_decoder.stop();
            reset_cast_nv12_pipeline(host.cast);
            break;
        }
        // The production encoder already accepted frame zero during startup.
        host.cast.frame_index = use_compute_nv12 ? 1 : 0;
        host.cast.audio_submitted_frames = 0;
        host.cast.lifecycle.activate_transport();
        host.cast.last_decoded_frame_id = 0;
        host.cast.last_decoded_generation = 0;
        host.cast.last_decoded_identity_valid = false;
        clear_cast_video_start_gate(host.cast);
        reset_cast_audio_alignment(host.cast);
        host.cast.next_frame_due = {};
        host.cast.active_backend = use_compute_nv12
            ? D3D11CastBackend::compute_nv12_a
            : D3D11CastBackend::compatibility_c;
        if (!use_compute_nv12
                && capability_report.reason
                    == D3D11CastBackendReason::probe_incomplete) {
            capability_report.reason = provisional_report.reason;
        }
        bool idle_ready{};
        if (use_compute_nv12) {
            idle_ready = static_cast<bool>(host.cast.last_nv12_frame);
        } else {
            idle_ready = host.cast.stage_renderer.compose_idle_frame();
            if (idle_ready) host.cast.stage_renderer.finalize_frame();
        }
        if (!idle_ready) {
            shutdown_cast_pipeline_resources(host.cast);
            continue;
        }
        const auto encoder_backend = host.cast.video_encoder.backend();
        if (host.cast.video_encoder.force_keyframe_support()
                == MfH264ForceKeyframeSupport::unsupported) {
            log_line("cast stream: force-keyframe control unavailable; "
                "fixed half-second GOP remains active");
        }
        if (encoder_backend == MfH264EncoderBackend::software) {
            log_line("cast stream: software H264 1080p30 fallback");
        } else if (profile.encoder_preference
                == MfH264EncoderPreference::hardware_readback_only) {
            log_line(profile.canvas.frame_rate_num == 60
                ? "cast stream: hardware-readback H264 1080p60 fallback"
                : "cast stream: hardware-readback H264 1080p30 fallback");
        } else {
            log_line(profile.canvas.frame_rate_num == 60
                ? "cast stream: hardware-direct H264 1080p60"
                : "cast stream: hardware-direct H264 1080p30");
        }
        capability_report.backend = host.cast.active_backend;
        capability_report.backend_active = true;
        switch (host.cast.video_encoder.force_keyframe_support()) {
        case MfH264ForceKeyframeSupport::unsupported:
            capability_report.force_keyframe_control =
                D3D11CastKeyframeControlStatus::unsupported;
            break;
        case MfH264ForceKeyframeSupport::supported:
            capability_report.force_keyframe_control =
                D3D11CastKeyframeControlStatus::supported;
            break;
        case MfH264ForceKeyframeSupport::failed:
            capability_report.force_keyframe_control =
                D3D11CastKeyframeControlStatus::failed;
            break;
        case MfH264ForceKeyframeSupport::unknown:
            capability_report.force_keyframe_control =
                D3D11CastKeyframeControlStatus::unknown;
            break;
        }
        {
            std::lock_guard lock(host.cast.capability_mutex);
            host.cast.capability_report = capability_report;
            host.cast.capability_report_valid = true;
        }
        host.cast.published_port.store(
            host.cast.backend.port(), std::memory_order_release);
        return true;
    }

        shutdown_cast_pipeline_resources(host.cast);
        return false;
    });

    if (!pipeline_started) {
        stop_cast_load_keepalive(cast);
        cast.lifecycle.stop();
    }
    return pipeline_started;
}

bool prepare_cast_pipeline(
        CastSession& cast,
        double takeover_position,
        bool prime_current_video) {
    return invoke_cast_worker(cast, [&]() -> bool {
            cast.program_clock.reset(
                takeover_position, cast_monotonic_seconds());
            cast.lifecycle.prepare_program(
                prime_current_video,
                takeover_position,
                std::chrono::steady_clock::now(),
                cast.program.playback_state->load(std::memory_order_relaxed)
                    == SHOW_STATE_PLAYING);
            return true;
        });
}

void enable_cast_pipeline(CastSession& cast) {
    {
        std::lock_guard lock(cast.worker_mutex);
        cast.worker_render_enabled = true;
        cast.next_frame_due = std::chrono::steady_clock::now();
    }
    SetEvent(cast.worker_wake_event);
}

double stop_cast_pipeline(CastSession& cast) {
    if (!cast.worker.joinable()) {
        return cast.program.valid()
            ? cast.program.published_position->load(std::memory_order_relaxed)
            : 0.0;
    }
    stop_cast_load_keepalive(cast);
    const auto restore_position = cast.program_clock.position_seconds();
    cast.frame_index = 0;
    cast.audio_submitted_frames = 0;
    cast.lifecycle.stop();
    cast.last_decoded_frame_id = 0;
    cast.last_decoded_generation = 0;
    cast.last_decoded_identity_valid = false;
    clear_cast_video_start_gate(cast);
    reset_cast_audio_alignment(cast);
    cast.audio_source_scratch.clear();
    cast.audio_output_scratch.clear();
    cast.next_frame_due = {};
    cast.program_clock.reset(restore_position, cast_monotonic_seconds());
    if (cast.program.valid()) {
        cast.program.published_position->store(
            restore_position, std::memory_order_relaxed);
        cast.program.published_buffering->store(
            false, std::memory_order_relaxed);
    }
    return restore_position;
}

static void render_cast_audio(CastExecutionContext& host,
        const kirakara::show::win32::StageCanvas& canvas,
        double stage_time,
        bool program_frame_ready) {
    if (!host.cast.audio_encoder.is_running()
            || canvas.frame_rate_num == 0
            || canvas.frame_rate_den == 0) {
        return;
    }

    const auto target_frames = static_cast<std::uint64_t>(
        (host.cast.frame_index + 1ULL)
            * static_cast<std::uint64_t>(
                CastPcmResampler::kOutputSampleRate)
            * canvas.frame_rate_den
            / canvas.frame_rate_num);
    if (target_frames <= host.cast.audio_submitted_frames) return;
    const auto output_frames = static_cast<std::size_t>(
        target_frames - host.cast.audio_submitted_frames);
    host.cast.audio_output_scratch.assign(
        output_frames * CastPcmResampler::kOutputChannels,
        std::int16_t{});

    const bool consume_program_audio = program_frame_ready
        && host.playback.state.load() == SHOW_STATE_PLAYING
        && !host.playback.video_buffering.load()
        && host.playback.audio.is_open();
    if (consume_program_audio) {
        const auto format = host.playback.audio.processed_pcm_format();
        if (format.valid()) {
            host.cast.audio_resampler.prepare_format(format);
            const auto timeline_revision =
                host.playback.video_timeline_revision;
            const auto desired_program_frame = std::max(0.0, stage_time)
                * static_cast<double>(format.sample_rate);
            const auto realign_threshold = kCastAudioRealignSeconds
                * static_cast<double>(format.sample_rate);
            if (!host.cast.audio_cursor_valid
                    || host.cast.audio_timeline_revision != timeline_revision
                    || std::abs(host.cast.audio_program_frame
                        - desired_program_frame) > realign_threshold) {
                host.cast.audio_resampler.reset();
                host.cast.audio_resampler.prepare_format(format);
                host.cast.audio_timeline_revision = timeline_revision;
                host.cast.audio_source_cursor = static_cast<std::uint64_t>(
                    std::floor(desired_program_frame));
                host.cast.audio_program_frame = desired_program_frame;
                host.cast.audio_cursor_valid = true;
            }

            const auto needed = host.cast.audio_resampler
                .source_frames_needed(output_frames);
            const auto channels = static_cast<std::size_t>(format.channels);
            host.cast.audio_source_scratch.resize(needed * channels);
            const auto read = host.playback.audio.read_processed_pcm_at(
                host.cast.audio_source_cursor,
                host.cast.audio_source_scratch.data(), needed);
            if (needed > 0 && read == needed) {
                host.cast.audio_resampler.append(
                    host.cast.audio_source_scratch.data(), read, format);
                host.cast.audio_resampler.render(
                    host.cast.audio_output_scratch.data(), output_frames);
                host.cast.audio_source_cursor += read;
                host.cast.audio_program_frame +=
                    static_cast<double>(output_frames)
                    * static_cast<double>(format.sample_rate)
                    / CastPcmResampler::kOutputSampleRate;
            } else {
                reset_cast_audio_alignment(host.cast);
            }
        }
    } else {
        reset_cast_audio_alignment(host.cast);
    }

    static_cast<void>(host.cast.audio_encoder.submit_pcm(
        host.cast.audio_output_scratch.data(),
        output_frames,
        host.cast.audio_submitted_frames));
    host.cast.audio_submitted_frames = target_frames;
}

static void render_cast_compute_nv12_frame(
        CastExecutionContext& host,
        const kirakara::show::win32::StageCanvas& canvas,
        const std::shared_ptr<const DecodedD3D11VideoFrame>& decoded,
        double stage_time,
        bool has_video,
        bool transition_preopen,
        bool start_buffer_ready,
        std::uint64_t video_revision,
        std::uint64_t missed_frames) {
    std::shared_ptr<const CastNormalizedVideoFrame> normalized_video;
    if (decoded && !transition_preopen && start_buffer_ready) {
        normalized_video = host.cast.video_normalizer.normalize(
            CastVideoFrameInput{
                decoded->texture(),
                decoded->format(),
                decoded->timestamp_seconds(),
                decoded->duration_seconds(),
                decoded->frame_id(),
                decoded->generation(),
                decoded->retention_token()});
    }

    // A video program becomes current only after its own normalized frame has
    // survived overlay composition and encoder queue admission. Until then an
    // immutable previous output remains on air. Audio-only programs must not
    // inherit that hold: they take over with the prebuilt black NV12 base.
    const bool source_frame_on_time = !has_video
        || static_cast<bool>(normalized_video);
    const bool hold_previous_program = has_video
        && !normalized_video
        && (host.cast.lifecycle.snapshot().holding_previous_output
            || host.cast.lifecycle.program_ready_for(video_revision));

    std::shared_ptr<const D3D11Nv12CompositedFrame> candidate;
    bool candidate_is_current_program{};
    if (hold_previous_program) {
        candidate = host.cast.last_nv12_frame;
    } else {
        const auto base = normalized_video
            ? normalized_video : host.cast.nv12_idle_frame;
        if (base) {
            static const kirakara::show::PreparedDocument empty_document;
            static const kirakara::show::SongTitleConfig empty_title;
            const auto& overlay_document = source_frame_on_time
                ? host.playback.document : empty_document;
            const auto& overlay_title = source_frame_on_time
                ? host.playback.config.song_title : empty_title;
            auto* title_renderer = source_frame_on_time
                    && host.cast.title_renderer_ready
                ? &host.cast.title_renderer : nullptr;
            if (host.cast.overlay_renderer.render(
                    overlay_document,
                    overlay_title,
                    stage_time,
                    host.playback.config.engine,
                    host.playback.config.style,
                    video_revision,
                    host.cast.lyric_renderer,
                    title_renderer)) {
                candidate = host.cast.nv12_overlay_compositor.compose(
                    D3D11Nv12OverlayInput{
                        base->texture(),
                        base->y_srv(),
                        base->uv_srv(),
                        base->retention_token(),
                        base->frame_id(),
                        base->generation()},
                    host.cast.overlay_renderer.frame(),
                    host.cast.frame_index);
                candidate_is_current_program = candidate
                    && source_frame_on_time;
            }
        }
        // Pool pressure or a device error must not create a transport hole.
        // Reusing the last already-composited NV12 frame is not a backend
        // fallback and does not claim that the new program is ready.
        if (!candidate) candidate = host.cast.last_nv12_frame;
    }

    const auto frame_duration_100ns = static_cast<std::int64_t>(
        10000000ULL * canvas.frame_rate_den / canvas.frame_rate_num);
    const auto frame_time_100ns = static_cast<std::int64_t>(
        host.cast.frame_index) * frame_duration_100ns;
    const bool request_program_keyframe = candidate_is_current_program
        && !host.cast.lifecycle.program_ready_for(video_revision);
    const bool video_submitted = candidate && candidate->texture
        && host.cast.video_encoder.submit_nv12_texture(
            candidate->texture,
            candidate->retention,
            frame_time_100ns,
            frame_duration_100ns,
            request_program_keyframe);
    if (video_submitted && candidate_is_current_program) {
        host.cast.last_nv12_frame = std::move(candidate);
        host.cast.lifecycle.mark_program_frame_committed(video_revision);
    }

    const auto program_frame_ready = source_frame_on_time
        && host.cast.lifecycle.program_ready_for(video_revision);
    if (host.cast.lifecycle.snapshot().start_gate_pending
            && program_frame_ready) {
        release_cast_video_start_gate(host);
        log_line("cast handoff: first frame ready; clock released");
    }
    const auto audio_chunk_start_time = cast_audio_chunk_start_time(
        stage_time,
        missed_frames,
        canvas.frame_rate_num,
        canvas.frame_rate_den);
    render_cast_audio(host, canvas, audio_chunk_start_time,
        program_frame_ready);
    ++host.cast.frame_index;
}

void render_cast_frame(CastSession& cast) {
    if (!cast.program.valid()) return;
    auto context = execution_context(cast);
    if (!context) return;
    auto& host = *context;
    auto& cast_renderer = host.cast.stage_renderer;
    auto& compositor = cast_renderer.compositor();
    const auto canvas = active_cast_canvas(host.cast);
    if (canvas.width == 0 || canvas.height == 0) return;
    if (!host.cast.backend.is_running()
            || !host.cast.video_encoder.is_running()
            || !host.cast.audio_encoder.is_running()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto frame_interval = std::chrono::duration_cast<
        std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(
                static_cast<double>(canvas.frame_rate_den)
                    / static_cast<double>(canvas.frame_rate_num)));
    if (host.cast.next_frame_due.time_since_epoch().count() == 0
            || now < host.cast.next_frame_due - frame_interval * 4) {
        host.cast.next_frame_due = now;
    }
    if (now < host.cast.next_frame_due) {
        return;
    }

    // Cast is a live transport, not an offline render. If the machine misses
    // one or more output slots, advance the transport clock instead of
    // rendering the overdue frames in a burst. Bursting stale PTS values makes
    // receivers alternate between catch-up playback and rebuffering.
    const auto missed_frames = static_cast<std::uint64_t>(
        (now - host.cast.next_frame_due) / frame_interval);
    if (missed_frames > 0) {
        host.cast.frame_index += missed_frames;
        host.cast.pipeline_diagnostics.increment(
            CastPipelineCounter::missed_output_slots, missed_frames);
    }
    host.cast.next_frame_due += frame_interval * (missed_frames + 1ULL);

    const auto state = host.playback.state.load();
    const bool playing = state == SHOW_STATE_PLAYING;
    double stage_time = host.cast.program_clock.position_seconds();
    // The five-second guard is diagnostic only. CastProgramClock remains
    // frozen until the decoder reaches high-water; timeout must never release
    // audio into a black/old video program.
    const auto lifecycle = host.cast.lifecycle.snapshot();
    if (lifecycle.start_gate_pending) {
        if (lifecycle.start_gate_started.time_since_epoch().count() != 0
                && now - lifecycle.start_gate_started
                    >= kCastVideoStartTimeout) {
            log_line("cast handoff: first frame still priming after 5 seconds");
            host.cast.lifecycle.suppress_start_gate_warning();
        }
    }
    const auto video_revision = host.playback.video_timeline_revision;
    const bool has_video = host.playback.current_media.loaded
        && !host.playback.current_media.video.empty();
    // While load() has published a source but play() has not arrived yet, keep
    // opening it and decode frame zero in paused state. An old complete Cast
    // frame remains on air when available; otherwise the idle frame stays
    // visible until the start gate performs the actual takeover.
    const bool transition_preopen = has_video
        && !host.cast.lifecycle.snapshot().start_gate_pending
        && (state == SHOW_STATE_STOPPED || state == SHOW_STATE_IDLE);
    auto decoder_state = MfD3D11PlaybackState::idle;
    if (has_video && playing) {
        decoder_state = MfD3D11PlaybackState::playing;
    } else if (has_video && state == SHOW_STATE_PAUSED) {
        decoder_state = MfD3D11PlaybackState::paused;
    } else if (transition_preopen) {
        decoder_state = MfD3D11PlaybackState::paused;
    }
    host.cast.video_decoder.synchronize(
        has_video ? host.playback.current_media.video : std::wstring{},
        video_revision,
        stage_time,
        decoder_state);

    const auto decoder_buffer = has_video
        ? host.cast.video_decoder.buffer_status(
            host.playback.current_media.video, video_revision)
        : MfD3D11VideoBufferStatus{};
    const auto clock_update = update_cast_program_clock(
        host, decoder_buffer, has_video, now);
    stage_time = clock_update.position_seconds;
    host.cast.lifecycle.apply_clock_state(clock_update.state);
    if (clock_update.state == CastProgramClockState::ended
            && state == SHOW_STATE_PLAYING) {
        if (host.cast.lifecycle.should_notify_ended(video_revision)) {
            PostMessageW(host.window, kCastMediaEndedMessage, 0,
                static_cast<LPARAM>(video_revision));
        }
    }
    const bool start_buffer_ready =
        !host.cast.lifecycle.snapshot().start_gate_pending
        || clock_update.state == CastProgramClockState::playing
        || clock_update.state == CastProgramClockState::ended;

    // Catalog media uses its separate audio track as the program clock.  When
    // a shorter background video reaches EOS, keep sampling its final frame
    // while lyrics and audio continue on the audio timeline.  Do not reuse the
    // previously composited Stage frame here: that would freeze the lyrics as
    // well as the video.
    const bool hold_video_tail_for_audio = !host.playback.video_master_clock
        && host.playback.audio.is_open()
        && decoder_buffer.frame_count > 0
        && decoder_buffer.availability
            == MfD3D11VideoAvailability::end_of_stream;
    const auto video_sample_time = hold_video_tail_for_audio
            && decoder_buffer.buffered_end_seconds > 0.0
        ? std::min(stage_time, decoder_buffer.buffered_end_seconds)
        : stage_time;

    std::shared_ptr<const DecodedD3D11VideoFrame> decoded;
    if (decoder_state != MfD3D11PlaybackState::idle) {
        const auto& source = host.playback.current_media.video;
        decoded = host.cast.video_decoder.texture_for(
            source, video_revision, video_sample_time);
        if (decoded && !hold_video_tail_for_audio
                && !video_frame_matches_program_time(
                decoded->timestamp_seconds(), stage_time)) {
            decoded.reset();
        }
    }
    if (cast_uses_compute_nv12(host.cast)) {
        render_cast_compute_nv12_frame(
            host,
            canvas,
            decoded,
            stage_time,
            has_video,
            transition_preopen,
            start_buffer_ready,
            video_revision,
            missed_frames);
        return;
    }
    bool composed_video = false;
    bool video_frame_on_time = !has_video;
    if (decoded && !transition_preopen && start_buffer_ready) {
        const auto normalize_started = CastPipelineDiagnostics::qpc_now();
        composed_video = cast_renderer.compose_video_texture(
            decoded->texture(), decoded->width(), decoded->height());
        const auto normalize_finished = CastPipelineDiagnostics::qpc_now();
        if (composed_video) {
            const bool repeated = host.cast.last_decoded_identity_valid
                && host.cast.last_decoded_generation == decoded->generation()
                && host.cast.last_decoded_frame_id == decoded->frame_id();
            if (repeated) {
                host.cast.pipeline_diagnostics.increment(
                    CastPipelineCounter::repeated_frames);
            }
            host.cast.last_decoded_frame_id = decoded->frame_id();
            host.cast.last_decoded_generation = decoded->generation();
            host.cast.last_decoded_identity_valid = true;
            host.cast.pipeline_diagnostics.record_duration(
                CastPipelineEvent::normalize,
                host.cast.frame_index,
                normalize_started,
                normalize_finished,
                decoded->frame_id(),
                static_cast<std::int64_t>(video_revision));
        }
        video_frame_on_time = composed_video;
    }
    const bool holding_last_complete_cast_frame = !composed_video
        && (host.cast.lifecycle.snapshot().holding_previous_output
            || host.cast.lifecycle.program_ready_for(video_revision));
    bool composed_new_cast_frame = composed_video;
    if (!composed_video && !holding_last_complete_cast_frame) {
        if (!cast_renderer.compose_idle_frame()) return;
        composed_new_cast_frame = true;
    }
    const bool title_overlay_active = host.cast.title_renderer_ready
        && kirakara::show::song_title_has_drawable_content(
            host.playback.config.song_title)
        && video_frame_on_time
        && kirakara::show::song_title_visible(
            stage_time, host.playback.config.song_title);
    const bool lyric_overlay_active = video_frame_on_time
        && !host.playback.document.lines.empty();
    if (!holding_last_complete_cast_frame
            && (title_overlay_active || lyric_overlay_active)) {
        const auto overlay_started = CastPipelineDiagnostics::qpc_now();
        auto& surface = compositor.overlay_surface();
        const auto frame = compositor.current_frame();
        if (frame.overlay_render_target) {
            // begin_frame() already cleared the previous program frame and
            // compose_*() populated its video/idle background.  Render the
            // title and lyrics over that existing frame; neither renderer may
            // clear the video underneath it.
            if (title_overlay_active
                    && host.cast.title_renderer.attach_native_target(
                        frame.overlay_render_target)) {
                static_cast<void>(host.cast.title_renderer.render(
                    host.playback.config.song_title,
                    stage_time,
                    host.playback.config.engine.fade_duration));
            }
            if (lyric_overlay_active
                    && host.cast.lyric_renderer.attach_native_target(
                        frame.overlay_render_target)) {
                static_cast<void>(host.cast.lyric_renderer.render_overlay(
                    host.playback.document,
                    stage_time,
                    host.playback.config.engine,
                    host.playback.config.style));
            }
        }
        host.cast.pipeline_diagnostics.record_duration(
            CastPipelineEvent::overlay_draw,
            host.cast.frame_index,
            overlay_started,
            CastPipelineDiagnostics::qpc_now(),
            0,
            (title_overlay_active ? 1 : 0)
                | (lyric_overlay_active ? 2 : 0));
    }
    if (composed_new_cast_frame) {
        // CastStageRenderer defers intermediate D2D flushes. Submit the fully
        // composed background/video/title/lyric frame exactly once before the
        // D3D11 video processor reads the BGRA texture for NV12 conversion.
        cast_renderer.finalize_frame();
    }

    const auto frame_duration_100ns = static_cast<std::int64_t>(
        10000000ULL * canvas.frame_rate_den / canvas.frame_rate_num);
    const auto frame_time_100ns = static_cast<std::int64_t>(
        host.cast.frame_index) * frame_duration_100ns;
    auto* stage_texture = active_cast_stage_texture(host.cast);
    const bool request_program_keyframe = video_frame_on_time
        && !holding_last_complete_cast_frame
        && !host.cast.lifecycle.program_ready_for(video_revision);
    const auto video_submitted = stage_texture && (
        holding_last_complete_cast_frame
            ? host.cast.video_encoder.submit_repeated_texture(
                stage_texture,
                frame_time_100ns,
                frame_duration_100ns)
            : host.cast.video_encoder.submit_texture(
                stage_texture,
                frame_time_100ns,
                frame_duration_100ns,
                request_program_keyframe));
    // The old frame remains the committed Cast program until the new texture
    // has both a small decoded runway and a successful encoder submission.
    // Native transition bookkeeping remains owned by the host thread.
    const auto timeline_revision = host.playback.video_timeline_revision;
    if (video_submitted && video_frame_on_time) {
        host.cast.lifecycle.mark_program_frame_committed(timeline_revision);
    }
    const auto program_frame_ready = video_frame_on_time
        && host.cast.lifecycle.program_ready_for(timeline_revision);
    // First frame of the newly loaded program is on screen: release the cast
    // handoff gate and start the audio clock from the held start position.
    // Both the video (frame 0) and the audio (cursor at start) then begin at
    // the same program time instead of mid-song. Transport PTS keeps its
    // monotonic timeline untouched.
    if (host.cast.lifecycle.snapshot().start_gate_pending
            && program_frame_ready) {
        // CastProgramClock already resumed from the buffered start position;
        // this only commits visual takeover of the new timeline.
        release_cast_video_start_gate(host);
        log_line("cast handoff: first frame ready; clock released");
    }
    // Dropped video slots must not become gaps in the AAC timeline. Submit one
    // continuous PCM chunk spanning those slots, starting at the corresponding
    // earlier program time, while the video transport remains live/current.
    const auto audio_chunk_start_time = cast_audio_chunk_start_time(
        stage_time,
        missed_frames,
        canvas.frame_rate_num,
        canvas.frame_rate_den);
    render_cast_audio(host, canvas, audio_chunk_start_time,
        program_frame_ready);
    ++host.cast.frame_index;
}

double cast_monotonic_seconds(
        std::chrono::steady_clock::time_point now) noexcept {
    return std::chrono::duration<double>(now.time_since_epoch()).count();
}

CastVideoBufferAvailability cast_buffer_availability(
        MfD3D11VideoAvailability availability) noexcept {
    switch (availability) {
    case MfD3D11VideoAvailability::idle:
        return CastVideoBufferAvailability::idle;
    case MfD3D11VideoAvailability::opening:
        return CastVideoBufferAvailability::opening;
    case MfD3D11VideoAvailability::ready:
        return CastVideoBufferAvailability::ready;
    case MfD3D11VideoAvailability::retryable_starvation:
        return CastVideoBufferAvailability::retryable_starvation;
    case MfD3D11VideoAvailability::end_of_stream:
        return CastVideoBufferAvailability::end_of_stream;
    case MfD3D11VideoAvailability::fatal_error:
        return CastVideoBufferAvailability::fatal_error;
    }
    return CastVideoBufferAvailability::fatal_error;
}

CastVideoBufferSnapshot cast_buffer_snapshot(
        const MfD3D11VideoBufferStatus& status) noexcept {
    CastVideoBufferSnapshot snapshot;
    snapshot.frame_count = status.frame_count;
    snapshot.start_seconds = status.buffered_start_seconds;
    snapshot.end_seconds = status.buffered_end_seconds;
    snapshot.source_duration_seconds = status.source_duration_seconds;
    snapshot.availability = cast_buffer_availability(status.availability);
    return snapshot;
}

[[nodiscard]] bool cast_uses_compute_nv12(
        const CastSession& cast) noexcept {
    return cast.active_backend == D3D11CastBackend::compute_nv12_a;
}

void reset_cast_nv12_pipeline(CastSession& cast) noexcept {
    cast.last_nv12_frame.reset();
    cast.nv12_idle_frame.reset();
    cast.nv12_overlay_compositor.reset();
    cast.overlay_renderer.reset();
    cast.video_normalizer.reset();
    cast.nv12_idle_pool.reset();
    cast.active_backend = D3D11CastBackend::compatibility_c;
}
