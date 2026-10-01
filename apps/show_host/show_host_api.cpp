#include "show_host_api.h"

#ifndef _WINSOCKAPI_
#include <winsock2.h>
#endif

#include "kirakara/show/config.hpp"
#include "kirakara/show/lrc_parser.hpp"
#include "kirakara/show/types.hpp"
#include "kirakara/show/win32/d3d11_texture_surface.hpp"
#include "kirakara/show/win32/lyric_renderer.hpp"
#include "kirakara/show/win32/offscreen_surface.hpp"

#include "host/host_utils.h"
#include "host/host_data.h"
#include "host/host_render.h"
#include "host/host_layout.h"
#include "host/host_commands.h"
#include "host/host_windows.h"

#include <windows.h>
#include <d2d1.h>
#include <mfapi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>
#include <thread>

#pragma comment(lib, "mf")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "ole32")
#pragma comment(lib, "shell32")

namespace {

std::wstring copied_text(const wchar_t* value) {
    return value ? std::wstring(value) : std::wstring{};
}

bool import_overlay_asset(const ShowHostStageOverlayAsset& input,
        kirakara::show::StageOverlayAsset& output) {
    if (input.struct_size != sizeof(ShowHostStageOverlayAsset)
            || input.reserved != 0) {
        return false;
    }
    output.cache_key = copied_text(input.cache_key);
    output.local_path = copied_text(input.local_path);
    output.content_revision = input.content_revision;
    return output.valid();
}

bool import_overlay_state(const ShowHostStageOverlayState& input,
        kirakara::show::StageOverlayState& output) {
    constexpr std::uint32_t known_flags = SHOW_STAGE_OVERLAY_QR_VISIBLE
        | SHOW_STAGE_OVERLAY_ANNOUNCEMENT_VISIBLE;
    if (input.struct_size != sizeof(ShowHostStageOverlayState)
            || (input.flags & ~known_flags) != 0) {
        return false;
    }
    output.revision = input.revision;
    output.qr.visible =
        (input.flags & SHOW_STAGE_OVERLAY_QR_VISIBLE) != 0;
    output.qr.payload = copied_text(input.qr_payload);
    output.announcement.visible =
        (input.flags & SHOW_STAGE_OVERLAY_ANNOUNCEMENT_VISIBLE) != 0;
    output.announcement.text = copied_text(input.announcement_text);
    return import_overlay_asset(input.qr_decoration, output.qr.decoration)
        && import_overlay_asset(input.announcement_decoration,
            output.announcement.decoration)
        && output.valid();
}

ShowHostCastCapabilityCheck export_capability_check(
        const D3D11CastCapabilityCheck& input) noexcept {
    ShowHostCastCapabilityCheck output{};
    output.hresult = input.hresult;
    output.attempted = input.attempted;
    output.supported = input.supported;
    return output;
}

std::uint8_t export_keyframe_control(
        D3D11CastKeyframeControlStatus status) noexcept {
    return static_cast<std::uint8_t>(status);
}

std::uint8_t export_keyframe_control(
        MfH264ForceKeyframeSupport status) noexcept {
    switch (status) {
    case MfH264ForceKeyframeSupport::unsupported:
        return SHOW_CAST_KEYFRAME_CONTROL_UNSUPPORTED;
    case MfH264ForceKeyframeSupport::supported:
        return SHOW_CAST_KEYFRAME_CONTROL_SUPPORTED;
    case MfH264ForceKeyframeSupport::failed:
        return SHOW_CAST_KEYFRAME_CONTROL_FAILED;
    case MfH264ForceKeyframeSupport::unknown:
        return SHOW_CAST_KEYFRAME_CONTROL_UNKNOWN;
    }
    return SHOW_CAST_KEYFRAME_CONTROL_UNKNOWN;
}

constexpr char kStageVisualProtocolRevision[] = "nt-keyed-latest-v1";
constexpr std::uint64_t kStageVisualCapabilities =
    SHOW_HOST_STAGE_VISUAL_CAP_NT_HANDLE
    | SHOW_HOST_STAGE_VISUAL_CAP_KEYED_MUTEX
    | SHOW_HOST_STAGE_VISUAL_CAP_LATEST_FRAME
    | SHOW_HOST_STAGE_VISUAL_CAP_NONBLOCKING_PRODUCER;

bool is_stage_visual_source(const StageTextureSourceState& source) noexcept {
    return source.transport
        == StagePreviewTransport::dcomp_visual_keyed_mutex;
}

int32_t create_stage_visual_source(
        ShowHostHandle handle, ShowHostStageVisualSource* output) {
    if (!output) return SHOW_HOST_STAGE_VISUAL_INVALID_ARGUMENT;
    *output = nullptr;
    auto* host = as_host(handle);
    if (!host) return SHOW_HOST_STAGE_VISUAL_INVALID_ARGUMENT;

    auto state = std::make_shared<StageTextureSourceState>();
    state->transport = StagePreviewTransport::dcomp_visual_keyed_mutex;
    state->mailbox = host->native_stage.unified_stage_renderer.subscribe();
    {
        std::lock_guard lock(host->native_stage.stage_texture_sources_mutex);
        host->native_stage.stage_texture_sources.emplace_back(state);
    }
    auto* holder = new (std::nothrow)
        std::shared_ptr<StageTextureSourceState>(std::move(state));
    if (!holder) return SHOW_HOST_STAGE_VISUAL_UNAVAILABLE;
    *output = holder;
    return SHOW_HOST_STAGE_VISUAL_SUCCESS;
}

void destroy_stage_visual_source(ShowHostStageVisualSource source) {
    auto* holder = static_cast<std::shared_ptr<StageTextureSourceState>*>(
        source);
    if (!holder || !*holder) return;
    {
        std::unique_lock lock((*holder)->mutex);
        if (!is_stage_visual_source(**holder)) return;
        (*holder)->active = false;
        (*holder)->visual_callback = nullptr;
        (*holder)->callback_context = nullptr;
        (*holder)->callback_notification_pending = false;
        (*holder)->visual_callback_condition.wait(lock, [&] {
            return (*holder)->visual_callbacks_in_flight == 0;
        });
        release_stage_preview_publication_resources(**holder);
        (*holder)->visual_notified_resource_generation = 0;
        (*holder)->publication_cadence.reset();
        (*holder)->mailbox = {};
    }
    delete holder;
}

void set_stage_visual_source_active(
        ShowHostStageVisualSource source, bool active) {
    auto* holder = static_cast<std::shared_ptr<StageTextureSourceState>*>(
        source);
    if (!holder || !*holder) return;
    std::unique_lock lock((*holder)->mutex);
    if (!is_stage_visual_source(**holder)) return;
    (*holder)->active = (*holder)->alive && active;
    (*holder)->publication_cadence.reset();
    if (!(*holder)->active) {
        (*holder)->callback_notification_pending = false;
        (*holder)->visual_callback_condition.wait(lock, [&] {
            return (*holder)->visual_callbacks_in_flight == 0;
        });
        // Abandon any frame left at consumer key 1. Never wait for a detached
        // consumer to return ownership; reactivation creates a fresh resource
        // generation starting at producer key 0.
        release_stage_preview_publication_resources(**holder);
        (*holder)->visual_notified_resource_generation = 0;
    } else {
        (*holder)->callback_notification_pending =
            (*holder)->visual_callback != nullptr;
    }
}

void set_stage_visual_source_frame_callback(
        ShowHostStageVisualSource source,
        ShowHostStageVisualFrameAvailableCallback callback,
        void* user_data) {
    auto* holder = static_cast<std::shared_ptr<StageTextureSourceState>*>(
        source);
    if (!holder || !*holder) return;
    std::unique_lock lock((*holder)->mutex);
    if (!is_stage_visual_source(**holder)) return;
    (*holder)->visual_callback = nullptr;
    (*holder)->callback_context = nullptr;
    (*holder)->callback_notification_pending = false;
    (*holder)->visual_callback_condition.wait(lock, [&] {
        return (*holder)->visual_callbacks_in_flight == 0;
    });
    (*holder)->visual_callback = (*holder)->alive ? callback : nullptr;
    (*holder)->callback_context =
        (*holder)->visual_callback ? user_data : nullptr;
    (*holder)->visual_notified_resource_generation = 0;
    (*holder)->callback_notification_pending = (*holder)->active
        && (*holder)->visual_callback != nullptr;
}

int32_t get_stage_visual_source_stats(
        ShowHostStageVisualSource source,
        ShowHostStageVisualStats* stats) {
    if (!stats || stats->struct_size != sizeof(*stats)
            || stats->abi_version != SHOW_HOST_STAGE_VISUAL_ABI_VERSION) {
        return SHOW_HOST_STAGE_VISUAL_VERSION_MISMATCH;
    }
    auto* holder = static_cast<std::shared_ptr<StageTextureSourceState>*>(
        source);
    if (!holder || !*holder) return SHOW_HOST_STAGE_VISUAL_INVALID_ARGUMENT;
    std::lock_guard lock((*holder)->mutex);
    if (!is_stage_visual_source(**holder)) {
        return SHOW_HOST_STAGE_VISUAL_INVALID_ARGUMENT;
    }
    ShowHostStageVisualStats result{};
    result.struct_size = sizeof(result);
    result.abi_version = SHOW_HOST_STAGE_VISUAL_ABI_VERSION;
    result.last_hresult = (*holder)->visual_last_hresult;
    result.published_frames = (*holder)->visual_published_frames;
    result.producer_busy_drops = (*holder)->visual_producer_busy_drops;
    result.callback_count = (*holder)->visual_callback_count;
    result.resource_recreations = (*holder)->visual_resource_recreations;
    result.resource_generation =
        (*holder)->publication_resource_generation;
    result.content_generation = (*holder)->publication_timing.generation;
    result.frame_id = (*holder)->publication_timing.frame_id;
    *stats = result;
    return SHOW_HOST_STAGE_VISUAL_SUCCESS;
}

}  // namespace

extern "C" {

SHOW_HOST_API ShowHostHandle show_host_create(void) {
    log_line("--- show_host stage create ---");
    auto* host = new ShowHost();
    host->thread.worker = std::thread(stage_thread_main, host);
    std::unique_lock lock(host->thread.ready_mutex);
    host->thread.ready_cv.wait(lock, [&] { return host->thread.ready; });
    if (host->thread.startup_failed) {
        lock.unlock();
        if (host->thread.worker.joinable()) host->thread.worker.join();
        delete host;
        return nullptr;
    }
    return host;
}

SHOW_HOST_API void show_host_destroy(ShowHostHandle handle) {
    auto* host = as_host(handle);
    if (!host) return;
    invalidate_stage_texture_sources(*host);
    if (host->playback.prepared_media) {
        auto state = host->playback.prepared_media;
        state->alive.store(false);
        state->generation.fetch_add(1);
        std::vector<std::thread> workers;
        {
            std::lock_guard lock(state->mutex);
            state->ready.reset();
            workers.swap(state->workers);
        }
        for (auto& worker : workers) {
            if (worker.joinable()) worker.join();
        }
    }
    if (host->thread.id != 0) {
        PostThreadMessageW(host->thread.id, WM_QUIT, 0, 0);
    } else if (host->window) {
        PostMessageW(host->window, WM_CLOSE, 0, 0);
    }
    if (host->thread.worker.joinable()) host->thread.worker.join();
    delete host;
    log_line("--- show_host stage destroy ---");
}

SHOW_HOST_API bool show_host_load(ShowHostHandle handle,
                                  const wchar_t* video_path,
                                  const wchar_t* lyric_path,
                                  const wchar_t* vocal_path,
                                  const wchar_t* accompaniment_path) {
    return show_host_load_with_clock(handle, video_path, lyric_path,
        vocal_path, accompaniment_path, SHOW_CLOCK_AUDIO_MASTER);
}

SHOW_HOST_API bool show_host_load_with_clock(
        ShowHostHandle handle,
        const wchar_t* video_path,
        const wchar_t* lyric_path,
        const wchar_t* vocal_path,
        const wchar_t* accompaniment_path,
        int clock_mode) {
    return show_host_load_with_options(handle, video_path, lyric_path,
        vocal_path, accompaniment_path, clock_mode, SHOW_TRANSITION_HARD);
}

SHOW_HOST_API bool show_host_load_with_options(
        ShowHostHandle handle,
        const wchar_t* video_path,
        const wchar_t* lyric_path,
        const wchar_t* vocal_path,
        const wchar_t* accompaniment_path,
        int clock_mode,
        int transition_mode) {
    auto* host = as_host(handle);
    if (!host || !host->window || !video_path) return false;
    LoadRequest request{
        video_path,
        lyric_path ? lyric_path : L"",
        vocal_path ? vocal_path : L"",
        accompaniment_path ? accompaniment_path : L"",
        clock_mode,
        transition_mode,
    };
    return SendMessageW(host->window, kHostLoadMessage, 0,
        reinterpret_cast<LPARAM>(&request)) == TRUE;
}

SHOW_HOST_API bool show_host_prepare_next(
        ShowHostHandle handle,
        const wchar_t* video_path,
        const wchar_t* lyric_path,
        const wchar_t* vocal_path,
        const wchar_t* accompaniment_path,
        int clock_mode) {
    auto* host = as_host(handle);
    if (!host || !host->window || !video_path) return false;
    LoadRequest request{
        video_path,
        lyric_path ? lyric_path : L"",
        vocal_path ? vocal_path : L"",
        accompaniment_path ? accompaniment_path : L"",
        clock_mode,
        SHOW_TRANSITION_HARD,
    };
    return SendMessageW(host->window, kHostPrepareNextMessage, 0,
        reinterpret_cast<LPARAM>(&request)) == TRUE;
}

SHOW_HOST_API void show_host_play(ShowHostHandle handle) {
    auto* host = as_host(handle);
    if (host && host->window) {
        PostMessageW(host->window, kHostPlayMessage, 0, 0);
    }
}

SHOW_HOST_API void show_host_pause(ShowHostHandle handle) {
    auto* host = as_host(handle);
    if (host && host->window) {
        PostMessageW(host->window, kHostPauseMessage, 0, 0);
    }
}

SHOW_HOST_API void show_host_stop(ShowHostHandle handle) {
    auto* host = as_host(handle);
    if (host && host->window) {
        PostMessageW(host->window, kHostStopMessage, 0, 0);
    }
}

SHOW_HOST_API void show_host_seek(ShowHostHandle handle, double seconds) {
    auto* host = as_host(handle);
    if (host && host->window) {
        SendMessageW(host->window, kHostSeekMessage, 0,
            reinterpret_cast<LPARAM>(&seconds));
    }
}

SHOW_HOST_API void show_host_set_volume(
        ShowHostHandle handle, int32_t volume_percent) {
    auto* host = as_host(handle);
    if (host && host->window) {
        SendMessageW(host->window, kHostVolumeMessage, 0,
            static_cast<LPARAM>(volume_percent));
    }
}

SHOW_HOST_API void show_host_set_key_semitones(
        ShowHostHandle handle, int32_t key_semitones) {
    auto* host = as_host(handle);
    if (host && host->window) {
        SendMessageW(host->window, kHostKeyMessage, 0,
            static_cast<LPARAM>(key_semitones));
    }
}

SHOW_HOST_API void show_host_set_audio_clock_offset(
        ShowHostHandle handle, double seconds) {
    auto* host = as_host(handle);
    if (host && host->window) {
        SendMessageW(host->window, kHostAudioClockOffsetMessage, 0,
            reinterpret_cast<LPARAM>(&seconds));
    }
}

SHOW_HOST_API bool show_host_set_audio_track(ShowHostHandle handle, int track) {
    auto* host = as_host(handle);
    if (!host || !host->window) return false;
    return SendMessageW(host->window, kHostAudioTrackMessage,
        static_cast<WPARAM>(track), 0) == TRUE;
}

SHOW_HOST_API void show_host_set_stage_visible(
    ShowHostHandle handle, bool visible) {
    auto* host = as_host(handle);
    if (host && host->window) {
        SendMessageW(host->window, kHostStageVisibleMessage,
            visible ? TRUE : FALSE, 0);
    }
}

SHOW_HOST_API void show_host_set_stage_window_rect(
    ShowHostHandle handle,
    std::int32_t x,
    std::int32_t y,
    std::uint32_t width,
    std::uint32_t height) {
    auto* host = as_host(handle);
    if (!host || !host->window) return;
    StageWindowRectRequest request{x, y, width, height};
    SendMessageW(host->window, kHostStageWindowRectMessage, 0,
        reinterpret_cast<LPARAM>(&request));
}

SHOW_HOST_API bool show_host_set_stage_d3d_device(
        ShowHostHandle handle, uintptr_t native_d3d11_device) {
    auto* host = as_host(handle);
    if (!host || !host->window || native_d3d11_device == 0) return false;
    return SendMessageW(
        host->window,
        kHostStageDeviceMessage,
        0,
        static_cast<LPARAM>(native_d3d11_device)) == TRUE;
}

SHOW_HOST_API ShowHostStageTextureSource
show_host_create_stage_texture_source(ShowHostHandle handle) {
    auto* host = as_host(handle);
    if (!host) return nullptr;
    auto state = std::make_shared<StageTextureSourceState>();
    state->mailbox = host->native_stage.unified_stage_renderer.subscribe();
    {
        std::lock_guard lock(host->native_stage.stage_texture_sources_mutex);
        host->native_stage.stage_texture_sources.emplace_back(state);
    }
    return new std::shared_ptr<StageTextureSourceState>(std::move(state));
}

SHOW_HOST_API void show_host_destroy_stage_texture_source(
        ShowHostStageTextureSource source) {
    auto* holder = static_cast<std::shared_ptr<StageTextureSourceState>*>(
        source);
    if (!holder) return;
    if (*holder) {
        std::lock_guard lock((*holder)->mutex);
        (*holder)->active = false;
        (*holder)->callback = nullptr;
        (*holder)->callback_context = nullptr;
        (*holder)->publication_texture.Reset();
        (*holder)->publication_shared_handle = 0;
        (*holder)->publication_ready = false;
        (*holder)->callback_notification_pending = false;
        (*holder)->publication_cadence.reset();
        (*holder)->mailbox = {};
    }
    delete holder;
}

SHOW_HOST_API void show_host_stage_texture_source_set_active(
        ShowHostStageTextureSource source, bool active) {
    auto* holder = static_cast<std::shared_ptr<StageTextureSourceState>*>(
        source);
    if (!holder || !*holder) return;
    std::lock_guard lock((*holder)->mutex);
    (*holder)->active = (*holder)->alive && active;
    (*holder)->publication_cadence.reset();
    if (!(*holder)->active) {
        (*holder)->publication_ready = false;
        (*holder)->callback_notification_pending = false;
    } else {
        (*holder)->callback_notification_pending =
            (*holder)->callback != nullptr;
    }
}

SHOW_HOST_API void show_host_stage_texture_source_set_frame_callback(
        ShowHostStageTextureSource source,
        ShowHostStageTextureFrameAvailableCallback callback,
        void* user_data) {
    auto* holder = static_cast<std::shared_ptr<StageTextureSourceState>*>(
        source);
    if (!holder || !*holder) return;
    std::lock_guard lock((*holder)->mutex);
    (*holder)->callback = (*holder)->alive ? callback : nullptr;
    (*holder)->callback_context = (*holder)->callback ? user_data : nullptr;
    (*holder)->callback_notification_pending = (*holder)->active
        && (*holder)->callback != nullptr;
}

SHOW_HOST_API bool show_host_stage_texture_source_acquire(
        ShowHostStageTextureSource source,
        ShowHostStageTextureFrame* frame) {
    if (!frame) return false;
    *frame = {};
    auto* holder = static_cast<std::shared_ptr<StageTextureSourceState>*>(
        source);
    if (!holder || !*holder) return false;
    std::lock_guard lock((*holder)->mutex);
    if (!(*holder)->alive || !(*holder)->active
            || !(*holder)->publication_ready
            || !(*holder)->publication_texture
            || (*holder)->publication_shared_handle == 0) {
        return false;
    }
    frame->shared_handle = (*holder)->publication_shared_handle;
    frame->lease_token = 0;
    frame->width = (*holder)->publication_profile.width;
    frame->height = (*holder)->publication_profile.height;
    frame->generation = (*holder)->publication_timing.generation;
    frame->frame_id = (*holder)->publication_timing.frame_id;
    return true;
}

SHOW_HOST_API void show_host_release_stage_texture_frame(
        uintptr_t lease_token) {
    delete reinterpret_cast<kirakara::show::StageFrameLease*>(lease_token);
}

SHOW_HOST_API int32_t show_host_get_stage_visual_api(
        uint32_t requested_abi_version,
        ShowHostStageVisualApi* api) {
    if (!api) return SHOW_HOST_STAGE_VISUAL_INVALID_ARGUMENT;
    if (requested_abi_version != SHOW_HOST_STAGE_VISUAL_ABI_VERSION
            || api->struct_size != sizeof(*api)) {
        return SHOW_HOST_STAGE_VISUAL_VERSION_MISMATCH;
    }
    ShowHostStageVisualApi result{};
    result.struct_size = sizeof(result);
    result.abi_version = SHOW_HOST_STAGE_VISUAL_ABI_VERSION;
    result.capabilities = kStageVisualCapabilities;
    result.protocol_revision = kStageVisualProtocolRevision;
    result.create_source = create_stage_visual_source;
    result.destroy_source = destroy_stage_visual_source;
    result.set_active = set_stage_visual_source_active;
    result.set_frame_callback = set_stage_visual_source_frame_callback;
    result.get_stats = get_stage_visual_source_stats;
    *api = result;
    return SHOW_HOST_STAGE_VISUAL_SUCCESS;
}

SHOW_HOST_API bool show_host_get_stage_frame_stats(
        ShowHostHandle handle,
        ShowHostStageFrameStats* stats) {
    auto* host = as_host(handle);
    if (!host || !stats
            || stats->struct_size != sizeof(ShowHostStageFrameStats)
            || stats->reserved != 0) {
        return false;
    }
    stats->produced_frames = host->native_stage.native_stage_produced_frames.load(
        std::memory_order_relaxed);
    stats->presented_frames = host->native_stage.native_stage_presented_frames.load(
        std::memory_order_relaxed);
    stats->dropped_frames = host->native_stage.native_stage_dropped_frames.load(
        std::memory_order_relaxed);
    return true;
}

SHOW_HOST_API bool show_host_get_cast_pipeline_stats(
        ShowHostHandle handle,
        ShowHostCastPipelineStats* stats) {
    auto* host = as_host(handle);
    if (!host || !stats
            || stats->struct_size != sizeof(ShowHostCastPipelineStats)
            || stats->reserved != 0) {
        return false;
    }
    const auto snapshot = host->cast.pipeline_diagnostics.snapshot();
    stats->session_generation = snapshot.session_generation;
    stats->qpc_frequency = snapshot.qpc_frequency;
    stats->decoded_frames = snapshot.counter(
        CastPipelineCounter::decoded_frames);
    stats->repeated_frames = snapshot.counter(
        CastPipelineCounter::repeated_frames);
    stats->video_processor_passes = snapshot.counter(
        CastPipelineCounter::video_processor_passes);
    stats->compute_dispatches = snapshot.counter(
        CastPipelineCounter::compute_dispatches);
    stats->gpu_copies = snapshot.counter(CastPipelineCounter::gpu_copies);
    stats->explicit_flushes = snapshot.counter(
        CastPipelineCounter::explicit_flushes);
    stats->encoder_drops = snapshot.counter(
        CastPipelineCounter::encoder_drops);
    stats->decoder_underflows = snapshot.counter(
        CastPipelineCounter::decoder_underflows);
    stats->decoder_recoveries = snapshot.counter(
        CastPipelineCounter::decoder_recoveries);
    stats->client_rebases = snapshot.counter(
        CastPipelineCounter::client_rebases);
    stats->texture_creations = snapshot.counter(
        CastPipelineCounter::texture_creations);
    stats->view_creations = snapshot.counter(
        CastPipelineCounter::view_creations);
    stats->missed_output_slots = snapshot.counter(
        CastPipelineCounter::missed_output_slots);
    static_assert(SHOW_CAST_ENCODER_INPUT_DISCONTINUITY
        == cast_encoder_input_diagnostic::discontinuity);
    static_assert(SHOW_CAST_ENCODER_INPUT_KEYFRAME_REQUESTED
        == cast_encoder_input_diagnostic::keyframe_requested);
    static_assert(SHOW_CAST_ENCODER_INPUT_KEYFRAME_APPLIED
        == cast_encoder_input_diagnostic::keyframe_applied);
    static_assert(SHOW_CAST_ENCODER_INPUT_KEYFRAME_UNSUPPORTED
        == cast_encoder_input_diagnostic::keyframe_unsupported);
    static_assert(SHOW_CAST_ENCODER_INPUT_KEYFRAME_FAILED
        == cast_encoder_input_diagnostic::keyframe_failed);
    static_assert(SHOW_CAST_EVENT_COUNT
        == static_cast<int>(CastPipelineEvent::count));
    constexpr auto event_count =
        static_cast<std::size_t>(SHOW_CAST_EVENT_COUNT);
    for (std::size_t index = 0; index < event_count; ++index) {
        const auto& source = snapshot.events[index];
        auto& destination = stats->events[index];
        destination.count = source.count;
        destination.last_frame_id = source.last_correlation_id;
        destination.last_qpc = source.last_qpc;
        destination.maximum_gap_qpc = source.maximum_gap_qpc;
        destination.total_duration_qpc = source.total_duration_qpc;
        destination.maximum_duration_qpc = source.maximum_duration_qpc;
    }
    return true;
}

SHOW_HOST_API bool show_host_set_cast_pipeline_trace_enabled(
        ShowHostHandle handle,
        bool enabled) {
    auto* host = as_host(handle);
    return host
        && host->cast.pipeline_diagnostics.set_trace_enabled(enabled);
}

SHOW_HOST_API bool show_host_get_cast_pipeline_trace_state(
        ShowHostHandle handle,
        ShowHostCastPipelineTraceState* state) {
    auto* host = as_host(handle);
    if (!host || !state
            || state->struct_size != sizeof(ShowHostCastPipelineTraceState)
            || state->reserved != 0) {
        return false;
    }
    const auto snapshot =
        host->cast.pipeline_diagnostics.trace_snapshot();
    state->enabled = snapshot.enabled ? 1U : 0U;
    state->capacity = static_cast<std::uint32_t>(
        CastPipelineDiagnostics::kTraceCapacity);
    state->latest_sequence = snapshot.latest_sequence;
    state->overwritten_events = snapshot.overwritten_events;
    state->dropped_events = snapshot.dropped_events;
    return true;
}

SHOW_HOST_API std::uint32_t show_host_read_cast_pipeline_trace(
        ShowHostHandle handle,
        ShowHostCastPipelineTraceRecord* records,
        std::uint32_t capacity,
        std::uint64_t after_sequence) {
    auto* host = as_host(handle);
    if (!host || !records || capacity == 0) return 0;
    const auto bounded_capacity = std::min<std::uint32_t>(
        capacity,
        static_cast<std::uint32_t>(
            CastPipelineDiagnostics::kTraceCapacity));
    auto temporary = std::unique_ptr<CastPipelineTraceRecord[]>(
        new (std::nothrow) CastPipelineTraceRecord[bounded_capacity]);
    if (!temporary) return 0;
    const auto count = host->cast.pipeline_diagnostics.read_trace(
        temporary.get(), bounded_capacity, after_sequence);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto& source = temporary[index];
        auto& destination = records[index];
        destination.sequence = source.sequence;
        destination.session_generation = source.session_generation;
        destination.qpc = source.qpc;
        destination.duration_qpc = source.duration_qpc;
        destination.correlation_id = source.correlation_id;
        destination.related_id = source.related_id;
        destination.value = source.value;
        destination.event = static_cast<std::uint32_t>(source.event);
        destination.thread_id = source.thread_id;
    }
    return count;
}

SHOW_HOST_API bool show_host_get_cast_capability_report(
        ShowHostHandle handle,
        ShowHostCastCapabilityReport* report) {
    auto* host = as_host(handle);
    if (!host || !report
            || report->struct_size != sizeof(ShowHostCastCapabilityReport)
            || report->reserved != 0) {
        return false;
    }
    *report = {};
    report->struct_size = sizeof(ShowHostCastCapabilityReport);
    std::lock_guard lock(host->cast.capability_mutex);
    const auto& input = host->cast.capability_report;
    report->adapter_luid = input.adapter_luid;
    report->vendor_id = input.vendor_id;
    report->device_id = input.device_id;
    report->feature_level = input.feature_level;
    report->windows_build = input.windows_build;
    report->backend = static_cast<std::uint32_t>(input.backend);
    report->backend_reason = static_cast<std::uint32_t>(input.reason);
    report->requires_encoder_copy = input.requires_encoder_copy;
    report->encoder_retains_samples_async =
        input.encoder_retains_samples_async;
    report->backend_active = input.backend_active;
    report->force_keyframe_control = export_keyframe_control(
        input.force_keyframe_control);
    // The encoder can learn force-keyframe support only after its first live
    // request. Preserve the pre-refactor API contract by overlaying that newer
    // thread-safe observation on the startup capability snapshot.
    const auto live_keyframe_control =
        host->cast.video_encoder.force_keyframe_support();
    if (live_keyframe_control != MfH264ForceKeyframeSupport::unknown) {
        report->force_keyframe_control = export_keyframe_control(
            live_keyframe_control);
    }
    report->report_valid = host->cast.capability_report_valid;
    report->device_healthy = export_capability_check(input.device_healthy);
    report->native_nv12_source = export_capability_check(
        input.native_nv12_source);
    report->nv12_plane_srvs = export_capability_check(input.nv12_plane_srvs);
    report->nv12_plane_uavs = export_capability_check(input.nv12_plane_uavs);
    report->uav_video_encoder_bind = export_capability_check(
        input.uav_video_encoder_bind);
    report->combined_uav_encoder_input = export_capability_check(
        input.combined_uav_encoder_input);
    report->compose_to_encoder_copy = export_capability_check(
        input.compose_to_encoder_copy);
    report->nv12_video_processor = export_capability_check(
        input.nv12_video_processor);
    report->dxgi_h264_encoder = export_capability_check(
        input.dxgi_h264_encoder);
    report->tracked_sample_release = export_capability_check(
        input.tracked_sample_release);
    report->even_420_geometry = export_capability_check(
        input.even_420_geometry);
    static_assert(SHOW_CAST_BACKEND_COMPATIBILITY_C
        == static_cast<int>(D3D11CastBackend::compatibility_c));
    static_assert(SHOW_CAST_BACKEND_NATIVE_NV12_VP_B
        == static_cast<int>(D3D11CastBackend::native_nv12_vp_b));
    static_assert(SHOW_CAST_BACKEND_COMPUTE_NV12_A
        == static_cast<int>(D3D11CastBackend::compute_nv12_a));
    static_assert(SHOW_CAST_BACKEND_REASON_COMPUTE_PATH_COMPLETE
        == static_cast<int>(D3D11CastBackendReason::compute_path_complete));
    static_assert(SHOW_CAST_BACKEND_REASON_INVALID_DEVICE
        == static_cast<int>(D3D11CastBackendReason::invalid_device));
    static_assert(SHOW_CAST_KEYFRAME_CONTROL_UNKNOWN
        == static_cast<int>(D3D11CastKeyframeControlStatus::unknown));
    static_assert(SHOW_CAST_KEYFRAME_CONTROL_UNSUPPORTED
        == static_cast<int>(D3D11CastKeyframeControlStatus::unsupported));
    static_assert(SHOW_CAST_KEYFRAME_CONTROL_SUPPORTED
        == static_cast<int>(D3D11CastKeyframeControlStatus::supported));
    static_assert(SHOW_CAST_KEYFRAME_CONTROL_FAILED
        == static_cast<int>(D3D11CastKeyframeControlStatus::failed));
    return true;
}

SHOW_HOST_API std::uint32_t show_host_get_active_video_decoder_owners(
        ShowHostHandle handle) {
    auto* host = as_host(handle);
    if (!host) return 0;
    return (host->output_transition.media_engine_video_source_active.load(
                std::memory_order_acquire) ? 1U : 0U)
        + (host->output_transition.cast_video_source_active.load(
                std::memory_order_acquire) ? 1U : 0U);
}

SHOW_HOST_API bool show_host_set_stage_overlay_state(
        ShowHostHandle handle,
        const ShowHostStageOverlayState* state) {
    auto* host = as_host(handle);
    if (!host || !host->window || !state) return false;
    StageOverlayRequest request;
    if (!import_overlay_state(*state, request.state)) return false;
    return SendMessageW(host->window, kHostStageOverlayStateMessage, 0,
        reinterpret_cast<LPARAM>(&request)) == TRUE;
}

SHOW_HOST_API std::uint64_t show_host_get_stage_overlay_revision(
        ShowHostHandle handle) {
    auto* host = as_host(handle);
    return host
        ? host->playback.stage_overlay_revision.load(std::memory_order_acquire) : 0;
}

SHOW_HOST_API double show_host_get_position(ShowHostHandle handle) {
    auto* host = as_host(handle);
    return host ? host->playback.position.load() : 0.0;
}

SHOW_HOST_API double show_host_get_duration(ShowHostHandle handle) {
    auto* host = as_host(handle);
    return host ? host->playback.duration.load() : 0.0;
}

SHOW_HOST_API int show_host_get_state(ShowHostHandle handle) {
    auto* host = as_host(handle);
    return host ? host->playback.state.load() : SHOW_STATE_IDLE;
}

SHOW_HOST_API bool show_host_is_buffering(ShowHostHandle handle) {
    auto* host = as_host(handle);
    return host ? host->playback.video_buffering.load() : false;
}

SHOW_HOST_API std::uint32_t show_host_get_width(ShowHostHandle handle) {
    auto* host = as_host(handle);
    return host ? host->frame_width.load() : 0;
}

SHOW_HOST_API std::uint32_t show_host_get_height(ShowHostHandle handle) {
    auto* host = as_host(handle);
    return host ? host->frame_height.load() : 0;
}

SHOW_HOST_API bool show_host_start_cast_stream(
    ShowHostHandle handle, std::uint16_t port) {
    auto* host = as_host(handle);
    if (!host || !host->window) return false;
    return SendMessageW(host->window, kHostStartCastStreamMessage,
        static_cast<WPARAM>(port), 0) == TRUE;
}

SHOW_HOST_API std::uint16_t show_host_get_cast_stream_port(
    ShowHostHandle handle) {
    auto* host = as_host(handle);
    return host
        ? host->cast.published_port.load(std::memory_order_acquire) : 0;
}

SHOW_HOST_API void show_host_stop_cast_stream(ShowHostHandle handle) {
    auto* host = as_host(handle);
    if (host && host->window) {
        SendMessageW(host->window, kHostStopCastStreamMessage, 0, 0);
    }
}

} // extern "C"
