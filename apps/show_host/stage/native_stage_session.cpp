// Native physical Stage and Flutter texture implementation.
// The function bodies are preserved from the former host orchestration unit.

#include "native_stage_session.h"

#include "../host/host_commands.h"
#include "../host/host_layout.h"
#include "../host/host_program.h"
#include "../host/host_render.h"
#include "../host/host_utils.h"

#include <cmath>
#include <dxgi1_2.h>
#include <utility>

namespace {

void record_stage_render_timings(
        NativeStagePipelineDiagnostics& diagnostics,
        const kirakara::show::win32::UnifiedStageRenderTimings& timings) {
    if (!diagnostics.enabled()) return;
    const auto record = [&diagnostics](NativeStagePipelineEvent event,
            std::uint64_t duration) {
        if (duration != 0) diagnostics.record_duration_ticks(event, duration);
    };
    record(NativeStagePipelineEvent::pool_acquire,
        timings.pool_acquire_qpc);
    record(NativeStagePipelineEvent::begin_clear,
        timings.begin_clear_qpc);
    record(NativeStagePipelineEvent::video_composite,
        timings.video_composite_qpc);
    record(NativeStagePipelineEvent::video_cache_copy,
        timings.video_cache_copy_qpc);
    record(NativeStagePipelineEvent::title_render,
        timings.title_render_qpc);
    record(NativeStagePipelineEvent::lyric_render,
        timings.lyric_render_qpc);
    record(NativeStagePipelineEvent::d2d_flush,
        timings.d2d_flush_qpc);
    record(NativeStagePipelineEvent::d3d_flush,
        timings.d3d_flush_qpc);
    diagnostics.increment(
        NativeStagePipelineCounter::d2d_flush_boundaries,
        timings.d2d_flush_count);
    diagnostics.increment(
        NativeStagePipelineCounter::d3d_flush_boundaries,
        timings.d3d_flush_count);
    record(NativeStagePipelineEvent::frame_publish,
        timings.frame_publish_qpc);
    if (timings.video_cache_refreshed) {
        diagnostics.increment(
            NativeStagePipelineCounter::video_cache_refreshes);
    }
    if (timings.video_cache_reused) {
        diagnostics.increment(
            NativeStagePipelineCounter::video_cache_reuses);
    }
}

void record_native_first_frame_if_pending(ShowHost& host) {
    const auto started =
        host.native_stage.native_video_start_diagnostics_qpc;
    if (!host.native_stage.native_video_start_pending || started == 0) return;
    host.native_stage.pipeline_diagnostics.record_duration(
        NativeStagePipelineEvent::first_frame_gate,
        started,
        NativeStagePipelineDiagnostics::qpc_now());
    host.native_stage.native_video_start_diagnostics_qpc = 0;
}

struct StageTextureDeliveryState {
    bool has_pending_frame{};
    bool requires_new_stage_frame{};
};

constexpr std::uint64_t kStageVisualProducerAcquireKey = 0;
constexpr std::uint64_t kStageVisualConsumerAcquireKey = 1;

bool is_stage_visual_source(const StageTextureSourceState& source) noexcept {
    return source.transport
        == StagePreviewTransport::dcomp_visual_keyed_mutex;
}

void release_stage_preview_publication(
        StageTextureSourceState& source) noexcept {
    source.publication_keyed_mutex.Reset();
    source.publication_texture.Reset();
    source.publication_shared_handle = 0;
    if (source.publication_nt_handle) {
        CloseHandle(source.publication_nt_handle);
        source.publication_nt_handle = nullptr;
    }
    source.publication_profile = {};
    source.publication_timing = {};
    source.publication_ready = false;
}

StageTextureDeliveryState stage_texture_delivery_state(ShowHost& host) {
    StageTextureDeliveryState result;
    for (const auto& source : live_stage_texture_sources(host)) {
        std::lock_guard lock(source->mutex);
        if (!source->alive || !source->active
                || (is_stage_visual_source(*source)
                    && !source->visual_callback)) {
            continue;
        }
        const bool pending = static_cast<bool>(source->mailbox.peek_latest());
        result.has_pending_frame = result.has_pending_frame || pending;
        const bool needs_delivery = !source->publication_ready
            || source->callback_notification_pending;
        result.requires_new_stage_frame = result.requires_new_stage_frame
            || (needs_delivery && !pending);
    }
    return result;
}

}  // namespace

void release_stage_preview_publication_resources(
        StageTextureSourceState& source) noexcept {
    release_stage_preview_publication(source);
}

bool publish_stage_texture_frame(
        StageTextureSourceState& source,
        const kirakara::show::StageFrame& frame,
        NativeStagePipelineDiagnostics& diagnostics) {
    auto* input = static_cast<ID3D11Texture2D*>(frame.native_resource);
    if (!input || frame.shared_handle == 0 || !frame.profile.valid()) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> input_device;
    input->GetDevice(input_device.GetAddressOf());
    if (!input_device) return false;

    bool recreate = !source.publication_texture
        || source.publication_profile.width != frame.profile.width
        || source.publication_profile.height != frame.profile.height
        || source.publication_profile.pixel_format
            != frame.profile.pixel_format;
    if (!recreate) {
        Microsoft::WRL::ComPtr<ID3D11Device> publication_device;
        source.publication_texture->GetDevice(
            publication_device.GetAddressOf());
        recreate = publication_device.Get() != input_device.Get();
    }

    if (recreate) {
        D3D11_TEXTURE2D_DESC description{};
        input->GetDesc(&description);
        description.Width = frame.profile.width;
        description.Height = frame.profile.height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.SampleDesc.Count = 1;
        description.SampleDesc.Quality = 0;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET
            | D3D11_BIND_SHADER_RESOURCE;
        description.CPUAccessFlags = 0;
        description.MiscFlags = is_stage_visual_source(source)
            ? D3D11_RESOURCE_MISC_SHARED_NTHANDLE
                | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX
            : D3D11_RESOURCE_MISC_SHARED;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> publication;
        const HRESULT create_result = input_device->CreateTexture2D(
            &description, nullptr, publication.GetAddressOf());
        if (FAILED(create_result)) {
            source.visual_last_hresult = create_result;
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyed_mutex;
        HANDLE nt_handle{};
        std::uintptr_t legacy_handle{};
        if (is_stage_visual_source(source)) {
            Microsoft::WRL::ComPtr<IDXGIResource1> shared_resource;
            HRESULT result = publication.As(&keyed_mutex);
            if (SUCCEEDED(result)) {
                result = publication.As(&shared_resource);
            }
            if (SUCCEEDED(result)) {
                result = shared_resource->CreateSharedHandle(
                    nullptr,
                    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                    nullptr,
                    &nt_handle);
            }
            if (FAILED(result) || !keyed_mutex || !nt_handle) {
                if (nt_handle) CloseHandle(nt_handle);
                source.visual_last_hresult = FAILED(result) ? result : E_FAIL;
                return false;
            }
        } else {
            Microsoft::WRL::ComPtr<IDXGIResource> shared_resource;
            HRESULT result = publication.As(&shared_resource);
            HANDLE shared_handle{};
            if (SUCCEEDED(result)) {
                result = shared_resource->GetSharedHandle(&shared_handle);
            }
            if (FAILED(result) || !shared_handle) {
                return false;
            }
            legacy_handle = reinterpret_cast<std::uintptr_t>(shared_handle);
        }

        release_stage_preview_publication(source);
        source.publication_texture = std::move(publication);
        source.publication_keyed_mutex = std::move(keyed_mutex);
        source.publication_shared_handle = legacy_handle;
        source.publication_nt_handle = nt_handle;
        source.publication_profile = frame.profile;
        if (is_stage_visual_source(source)) {
            source.publication_resource_generation =
                source.next_resource_generation++;
            if (source.next_resource_generation == 0) {
                source.next_resource_generation = 1;
            }
            ++source.visual_resource_recreations;
            source.visual_last_hresult = S_OK;
        }
    }

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    input_device->GetImmediateContext(context.GetAddressOf());
    if (!context) return false;

    if (is_stage_visual_source(source)) {
        if (!source.publication_keyed_mutex
                || !source.publication_nt_handle) {
            source.visual_last_hresult = E_NOINTERFACE;
            return false;
        }
        const HRESULT acquire_result =
            source.publication_keyed_mutex->AcquireSync(
                kStageVisualProducerAcquireKey, 0);
        if (acquire_result == WAIT_TIMEOUT) {
            ++source.visual_producer_busy_drops;
            source.visual_last_hresult = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
            return false;
        }
        if (FAILED(acquire_result)) {
            source.visual_last_hresult = acquire_result;
            return false;
        }

        {
            NativeStagePipelineDurationScope timing(
                diagnostics, NativeStagePipelineEvent::preview_copy);
            context->CopyResource(source.publication_texture.Get(), input);
        }
        // ReleaseSync transfers ownership only after prior commands touching
        // the shared surface are submitted/resolved by the D3D runtime. This
        // is the synchronization boundary; an unconditional context Flush is
        // neither needed nor allowed on the composed preview path.
        const HRESULT release_result =
            source.publication_keyed_mutex->ReleaseSync(
                kStageVisualConsumerAcquireKey);
        if (FAILED(release_result)) {
            source.visual_last_hresult = release_result;
            return false;
        }
        source.visual_last_hresult = S_OK;
        ++source.visual_published_frames;
    } else {
        {
            NativeStagePipelineDurationScope timing(
                diagnostics, NativeStagePipelineEvent::preview_copy);
            context->CopyResource(source.publication_texture.Get(), input);
        }
        // The stock Flutter external-texture fallback has no explicit shared
        // synchronization primitive. Preserve its existing submission
        // boundary until that independent compatibility path is retired.
        {
            NativeStagePipelineDurationScope timing(
                diagnostics, NativeStagePipelineEvent::preview_flush);
            context->Flush();
        }
    }
    source.publication_timing = frame.timing;
    source.publication_ready = true;
    trace_native_stage_preview_publication(
        frame.timing.generation, frame.timing.frame_id);
    diagnostics.increment(
        NativeStagePipelineCounter::preview_publications);
    return true;
}

void notify_stage_texture_sources(
        ShowHost& host, bool physical_stage_active) {
    const auto sources = live_stage_texture_sources(host);
    if (sources.empty()) return;
    static_cast<void>(physical_stage_active);
    // The controller Preview is currently a native DirectComposition Visual,
    // so let it follow every produced Stage frame. Keep the former cadence
    // gate commented in place for a quick rollback while this is evaluated.
    // const auto now = StageTexturePublicationCadence::Clock::now();
    // const auto preview_fps =
    //     host.native_stage.preview_load_policy.preview_fps();
    for (const auto& source : sources) {
        StageTextureFrameAvailableCallback callback{};
        ShowHostStageVisualFrameAvailableCallback visual_callback{};
        void* context{};
        std::uintptr_t shared_handle{};
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint64_t generation{};
        std::uint64_t frame_id{};
        ShowHostStageVisualFrame visual_frame{};
        {
            std::lock_guard lock(source->mutex);
            if (!source->alive || !source->active
                    || (is_stage_visual_source(*source)
                        && !source->visual_callback)) {
                continue;
            }
            // if (!source->publication_cadence.should_publish(
            //         physical_stage_active, preview_fps, now)) {
            //     host.native_stage.pipeline_diagnostics.increment(
            //         NativeStagePipelineCounter::preview_throttled_frames);
            //     continue;
            // }
            auto frame = source->mailbox.take_latest();
            if (!frame) {
                source->publication_cadence.reset();
                continue;
            }
            if (!publish_stage_texture_frame(*source, *frame.get(),
                    host.native_stage.pipeline_diagnostics)) {
                source->publication_cadence.reset();
                continue;
            }
            callback = source->callback;
            visual_callback = source->visual_callback;
            context = source->callback_context;
            shared_handle = source->publication_shared_handle;
            width = source->publication_profile.width;
            height = source->publication_profile.height;
            generation = source->publication_timing.generation;
            frame_id = source->publication_timing.frame_id;
            if (is_stage_visual_source(*source) && visual_callback) {
                visual_frame.struct_size = sizeof(visual_frame);
                visual_frame.abi_version = SHOW_HOST_STAGE_VISUAL_ABI_VERSION;
                visual_frame.flags =
                    source->visual_notified_resource_generation
                            != source->publication_resource_generation
                        ? SHOW_HOST_STAGE_VISUAL_FRAME_RESOURCE_CHANGED
                        : 0;
                visual_frame.sync_type =
                    SHOW_HOST_STAGE_VISUAL_SYNC_KEYED_MUTEX;
                visual_frame.shared_nt_handle = reinterpret_cast<std::uintptr_t>(
                    source->publication_nt_handle);
                visual_frame.width = width;
                visual_frame.height = height;
                visual_frame.dxgi_format = DXGI_FORMAT_B8G8R8A8_UNORM;
                visual_frame.resource_generation =
                    source->publication_resource_generation;
                visual_frame.content_generation = generation;
                visual_frame.frame_id = frame_id;
                visual_frame.consumer_acquire_key =
                    kStageVisualConsumerAcquireKey;
                visual_frame.consumer_release_key =
                    kStageVisualProducerAcquireKey;
                source->visual_notified_resource_generation =
                    source->publication_resource_generation;
                ++source->visual_callback_count;
                ++source->visual_callbacks_in_flight;
            }
            source->callback_notification_pending = false;
        }
        if (visual_callback) {
            trace_native_stage_preview_callback(generation, frame_id);
            NativeStagePipelineDurationScope timing(
                host.native_stage.pipeline_diagnostics,
                NativeStagePipelineEvent::preview_callback);
            visual_callback(context, &visual_frame);
            {
                std::lock_guard lock(source->mutex);
                --source->visual_callbacks_in_flight;
            }
            source->visual_callback_condition.notify_all();
            host.native_stage.pipeline_diagnostics.increment(
                NativeStagePipelineCounter::preview_callbacks);
        } else if (callback) {
            trace_native_stage_preview_callback(generation, frame_id);
            NativeStagePipelineDurationScope timing(
                host.native_stage.pipeline_diagnostics,
                NativeStagePipelineEvent::preview_callback);
            callback(context, shared_handle, width, height,
                generation, frame_id);
            host.native_stage.pipeline_diagnostics.increment(
                NativeStagePipelineCounter::preview_callbacks);
        }
    }
}

void invalidate_stage_texture_sources(ShowHost& host) {
    for (const auto& source : live_stage_texture_sources(host)) {
        std::unique_lock lock(source->mutex);
        source->alive = false;
        source->active = false;
        source->callback = nullptr;
        source->visual_callback = nullptr;
        source->callback_context = nullptr;
        source->callback_notification_pending = false;
        source->visual_callback_condition.wait(lock, [&] {
            return source->visual_callbacks_in_flight == 0;
        });
        release_stage_preview_publication(*source);
        source->visual_notified_resource_generation = 0;
        source->publication_cadence.reset();
        source->mailbox = {};
    }
}

void rebind_stage_texture_sources(ShowHost& host) {
    for (const auto& source : live_stage_texture_sources(host)) {
        std::lock_guard lock(source->mutex);
        if (!source->alive) continue;
        release_stage_preview_publication(*source);
        source->callback_notification_pending = source->active
            && (source->callback != nullptr
                || source->visual_callback != nullptr);
        source->visual_notified_resource_generation = 0;
        source->publication_cadence.reset();
        source->mailbox = host.native_stage.unified_stage_renderer.subscribe();
    }
}

bool replace_unified_stage_renderer(ShowHost& host,
        const kirakara::show::StageOutputProfile& profile) {
    auto* device = host.native_stage.preferred_stage_device.Get();
    if (!device) {
        device = static_cast<ID3D11Device*>(
            host.native_stage.unified_stage_renderer.native_d3d_device());
    }

    kirakara::show::win32::UnifiedStageRenderer replacement;
    if (!replacement.configure(
            profile,
            kUnifiedStageFramePoolCapacity,
            device)) {
        return false;
    }

    // A Flutter descriptor may still own an immutable lease from the previous
    // frame pool. Replacing the renderer lets that pool retire naturally;
    // reconfiguring it in place would fail until Flutter asks for another
    // descriptor, creating a permanent no-new-frame cycle.
    host.native_stage.unified_stage_renderer.retire();
    host.native_stage.unified_stage_renderer = std::move(replacement);
    rebind_stage_texture_sources(host);
    return true;
}

void reset_unified_stage_device_pipeline(
        ShowHost& host, bool release_preferred_device) {
    host.native_stage.stage_video_decoder.stop();
    host.native_stage.stage_video_decoder_started = false;
    host.native_stage.standby_video_decoder.stop();
    host.native_stage.standby_video_decoder_started = false;
    host.native_stage.standby_video_ready = false;
    host.native_stage.standby_video_timeline_revision = 0;
    host.native_stage.standby_prepare_started_qpc = 0;
    host.native_stage.decoded_frame_identity_valid = false;
    release_unified_native_stage_frames(host);
    host.native_stage.unified_stage_presenter.shutdown();
    host.native_stage.unified_stage_renderer.retire();
    host.native_stage.unified_stage_renderer =
        kirakara::show::win32::UnifiedStageRenderer{};
    rebind_stage_texture_sources(host);
    if (release_preferred_device) {
        host.native_stage.preferred_stage_device.Reset();
    }
}

bool has_native_stage_window_output(ShowHost& host) {
    return has_physical_stage_output(host);
}

bool same_stage_profile(
        const kirakara::show::StageOutputProfile& left,
        const kirakara::show::StageOutputProfile& right) {
    return left.role == right.role
        && left.width == right.width
        && left.height == right.height
        && left.refresh_rate_num == right.refresh_rate_num
        && left.refresh_rate_den == right.refresh_rate_den
        && left.pixel_format == right.pixel_format;
}

kirakara::show::StageOutputProfile desired_native_stage_profile(
        ShowHost& host) {
    if (has_physical_stage_output(host)) {
        const auto rect = active_stage_rect(host);
        return kirakara::show::StageOutputProfile{
            kirakara::show::StageOutputRole::physical_display,
            static_cast<std::uint32_t>(std::max<LONG>(
                1, rect.right - rect.left)),
            static_cast<std::uint32_t>(std::max<LONG>(
                1, rect.bottom - rect.top)),
            // Stage content is authored and evaluated at 60 fps. Scanout may
            // repeat these frames on a faster display, but changing monitor
            // refresh rate must not rebuild the renderer or video decoder.
            60,
            1,
            kirakara::show::StagePixelFormat::bgra8,
        };
    }
    return kirakara::show::StageOutputProfile{
        kirakara::show::StageOutputRole::controller_preview,
        1920,
        1080,
        60,
        1,
        kirakara::show::StagePixelFormat::bgra8,
    };
}

void release_unified_native_stage_frames(ShowHost& host) {
    host.native_stage.unified_stage_presenter.release_frame();
    host.native_stage.native_presentation_holds_frame = false;
    host.native_stage.native_last_stage_frame = {};
    host.native_stage.native_stage_mailbox = {};
    host.native_stage.native_stage_generation_valid = false;
    host.native_stage.native_stage_configured = false;
    host.native_stage.frame_scheduler.invalidate();
    host.native_stage.force_stage_redraw = true;
}

bool configure_unified_native_stage(ShowHost& host) {
    if (has_ts_stage_output(host)) return false;
    if (has_native_stage_window_output(host) && !host.stage_surface_window) {
        return false;
    }
    const auto profile = desired_native_stage_profile(host);
    const bool profile_matches = host.native_stage.native_stage_configured
        && host.native_stage.unified_stage_renderer.configured()
        && same_stage_profile(
            host.native_stage.unified_stage_renderer.profile(), profile);
    if (!profile_matches) {
        release_unified_native_stage_frames(host);
        if (!replace_unified_stage_renderer(host, profile)) {
            log_line("Unified native Stage profile configuration failed");
            return false;
        }
        host.native_stage.native_stage_mailbox =
            host.native_stage.unified_stage_renderer.subscribe();
        host.native_stage.native_stage_configured = true;
    }

    auto* device = host.native_stage.unified_stage_renderer.native_d3d_device();
    if (!device) return false;
    if (!host.native_stage.stage_video_decoder_started) {
        host.native_stage.stage_video_decoder_started =
            host.native_stage.stage_video_decoder.start(device);
    }
    if (!host.native_stage.stage_video_decoder_started) {
        log_line("Unified native Stage device initialization failed");
        release_unified_native_stage_frames(host);
        return false;
    }
    if (!has_native_stage_window_output(host)) {
        return true;
    }
    if (!host.native_stage.unified_stage_presenter.initialize(host.stage_surface_window, device)) {
        log_line("Unified native Stage presenter initialization failed");
        release_unified_native_stage_frames(host);
        return false;
    }
    RECT client{};
    GetClientRect(host.stage_surface_window, &client);
    const auto width = static_cast<std::uint32_t>(
        std::max<LONG>(1, client.right - client.left));
    const auto height = static_cast<std::uint32_t>(
        std::max<LONG>(1, client.bottom - client.top));
    if (!host.native_stage.unified_stage_presenter.resize(width, height)) {
        log_line("Unified native Stage swap chain resize failed");
        return false;
    }
    ShowWindow(host.stage_surface_window, SW_SHOWNOACTIVATE);
    return true;
}

bool has_requested_native_stage_output(ShowHost& host) {
    return !has_ts_stage_output(host)
        && (has_physical_stage_output(host)
            || has_active_stage_texture_source(host));
}

bool prime_native_stage_video(ShowHost& host, double position) {
    if (!has_requested_native_stage_output(host)
            || !host.playback.current_media.loaded
            || host.playback.current_media.video.empty()
            || !configure_unified_native_stage(host)) {
        return false;
    }
    // if (!host.playback.player.has_future_data()) return false;
    // Gate on the Stage decoder producing its first frame, not on the
    // MediaEngine clock.  The clock player (video-master) reaches
    // HAVE_FUTURE_DATA much later on external HTTP sources; gating on it made
    // handle_play skip the gate, play the clock immediately, and later
    // handle_video_can_play seek audio/video to the drifted clock position
    // (~0.3s in) — the mid-song start observed on external links.  The Stage
    // decoder retries open/seek until the source is usable, and
    // kNativeVideoStartTimeout still releases the gate as a fallback, so a
    // broken source cannot hold playback forever.
    host.native_stage.stage_video_decoder.synchronize(
        host.playback.current_media.video,
        host.playback.video_timeline_revision.load(std::memory_order_relaxed),
        position,
        MfD3D11PlaybackState::paused);
    return true;
}

void request_native_stage_standby_video(
        ShowHost& host,
        const std::wstring& source,
        std::uint64_t prepare_generation) {
    const bool same_source = host.native_stage.standby_video_requested
        && host.native_stage.standby_video_source == source;
    if (!same_source && host.native_stage.standby_video_decoder_started) {
        host.native_stage.standby_video_decoder.suspend();
    }
    host.native_stage.pipeline_diagnostics.increment(
        NativeStagePipelineCounter::standby_requests);
    host.native_stage.standby_video_source = source;
    host.native_stage.standby_prepare_generation = prepare_generation;
    host.native_stage.standby_prepare_started_qpc =
        host.native_stage.pipeline_diagnostics.enabled()
        ? NativeStagePipelineDiagnostics::qpc_now() : 0;
    host.native_stage.standby_video_requested = !source.empty();
    if (!same_source || source.empty()) {
        host.native_stage.standby_video_ready = false;
        host.native_stage.standby_video_timeline_revision = 0;
    }
}

void suspend_native_stage_standby_video(
        ShowHost& host, bool preserve_request) {
    const bool cancelled = !preserve_request
        && host.native_stage.standby_video_requested;
    if (host.native_stage.standby_video_decoder_started) {
        // This is deliberately non-blocking. A synchronous stop/join here can
        // freeze the Host message thread while an MF source read is pending,
        // which would regress Cast connection latency. The idle directive
        // drops the frame queue and closes the SourceReader on its worker.
        host.native_stage.standby_video_decoder.suspend();
    }
    host.native_stage.standby_video_ready = false;
    host.native_stage.standby_video_timeline_revision = 0;
    host.native_stage.standby_prepare_started_qpc = 0;
    if (preserve_request) return;
    host.native_stage.standby_video_requested = false;
    host.native_stage.standby_video_source.clear();
    host.native_stage.standby_prepare_generation = 0;
    if (cancelled) {
        host.native_stage.pipeline_diagnostics.increment(
            NativeStagePipelineCounter::standby_cancellations);
    }
}

namespace {

void service_native_stage_standby_video(ShowHost& host) {
    if (!host.native_stage.standby_video_requested
            || host.native_stage.standby_video_source.empty()
            || has_ts_stage_output(host)
            || !has_requested_native_stage_output(host)) {
        return;
    }
    auto* device = host.native_stage.unified_stage_renderer.native_d3d_device();
    if (!device) return;
    if (!host.native_stage.standby_video_decoder_started) {
        const auto config = native_stage_standby_video_decoder_config();
        host.native_stage.standby_video_decoder_started =
            host.native_stage.standby_video_decoder.start(device, config);
        if (!host.native_stage.standby_video_decoder_started) return;
        host.native_stage.pipeline_diagnostics.increment(
            NativeStagePipelineCounter::standby_decoder_starts);
    }

    const auto target_revision =
        host.playback.video_timeline_revision.load(std::memory_order_relaxed)
        + 1;
    if (host.native_stage.standby_video_timeline_revision
            != target_revision) {
        if (host.native_stage.pipeline_diagnostics.enabled()
                && (host.native_stage.standby_video_timeline_revision != 0
                    || host.native_stage.standby_prepare_started_qpc == 0)) {
            host.native_stage.standby_prepare_started_qpc =
                NativeStagePipelineDiagnostics::qpc_now();
        }
        host.native_stage.standby_video_timeline_revision = target_revision;
        host.native_stage.standby_video_ready = false;
    }
    if (host.native_stage.standby_video_ready) return;

    host.native_stage.standby_video_decoder.synchronize(
        host.native_stage.standby_video_source,
        target_revision,
        0.0,
        MfD3D11PlaybackState::paused);
    const auto frame = host.native_stage.standby_video_decoder.texture_for(
        host.native_stage.standby_video_source,
        target_revision,
        0.0);
    const bool ready = frame
        && video_frame_matches_program_time(
            frame->timestamp_seconds(), 0.0);
    if (!ready) return;
    host.native_stage.standby_video_ready = true;
    host.native_stage.pipeline_diagnostics.increment(
        NativeStagePipelineCounter::standby_ready_frames);
    if (host.native_stage.standby_prepare_started_qpc != 0) {
        host.native_stage.pipeline_diagnostics.record_duration(
            NativeStagePipelineEvent::standby_prime,
            host.native_stage.standby_prepare_started_qpc,
            NativeStagePipelineDiagnostics::qpc_now());
    }
}

}  // namespace

bool promote_native_stage_standby_video(
        ShowHost& host,
        const std::wstring& source,
        std::uint64_t prepare_generation,
        std::uint64_t timeline_revision) {
    if (has_ts_stage_output(host)
            || !host.native_stage.standby_video_requested
            || !host.native_stage.standby_video_ready
            || !host.native_stage.standby_video_decoder_started
            || host.native_stage.standby_video_source != source
            || host.native_stage.standby_prepare_generation
                != prepare_generation
            || host.native_stage.standby_video_timeline_revision
                != timeline_revision) {
        return false;
    }

    host.native_stage.stage_video_decoder.swap(
        host.native_stage.standby_video_decoder);
    std::swap(host.native_stage.stage_video_decoder_started,
        host.native_stage.standby_video_decoder_started);
    // The previous active decoder now occupies the standby slot. Retire its
    // old directive asynchronously while preserving the worker for the next
    // prepare request on the same device.
    if (host.native_stage.standby_video_decoder_started) {
        host.native_stage.standby_video_decoder.suspend();
    }
    host.native_stage.standby_video_requested = false;
    host.native_stage.standby_video_ready = false;
    host.native_stage.standby_video_source.clear();
    host.native_stage.standby_prepare_generation = 0;
    host.native_stage.standby_video_timeline_revision = 0;
    host.native_stage.standby_prepare_started_qpc = 0;
    host.native_stage.decoded_frame_identity_valid = false;
    host.native_stage.force_stage_redraw = true;
    host.native_stage.pipeline_diagnostics.increment(
        NativeStagePipelineCounter::standby_promotions);
    return true;
}

void clear_native_video_start_gate(ShowHost& host) {
    host.native_stage.native_video_start_pending = false;
    host.native_stage.native_video_start_position = 0.0;
    host.native_stage.native_video_start_started = {};
    host.native_stage.native_video_start_diagnostics_qpc = 0;
    host.native_stage.native_output_recovery_pending = false;
}

void begin_native_output_recovery(ShowHost& host) {
    host.native_stage.force_stage_redraw = true;
    if (host.playback.state.load() != SHOW_STATE_PLAYING
            || !host.playback.current_media.loaded) {
        return;
    }
    const auto recovery_position = playback_time(host);
    host.native_stage.native_video_start_pending = true;
    host.native_stage.native_video_start_position = recovery_position;
    host.native_stage.native_video_start_started = std::chrono::steady_clock::now();
    host.native_stage.native_video_start_diagnostics_qpc =
        host.native_stage.pipeline_diagnostics.enabled()
        ? NativeStagePipelineDiagnostics::qpc_now() : 0;
    host.native_stage.native_output_recovery_pending = true;
    static_cast<void>(host.playback.audio->pause());
    pause_video_outputs(host);
    host.playback.position.store(recovery_position);
}

void release_native_video_start_gate(ShowHost& host) {
    if (!host.native_stage.native_video_start_pending) return;
    const auto start_position = host.native_stage.native_video_start_position;
    clear_native_video_start_gate(host);

    if (has_ts_stage_output(host)) {
        // The Cast program clock performs its own high-water handoff. A
        // native gate left over while the output lease changes must not start
        // MediaEngine or the audio clock for a few milliseconds.
        host.playback.position.store(start_position);
        return;
    }

    seek_video_outputs(host, start_position);
    if (host.playback.audio->is_open()) {
        static_cast<void>(host.playback.audio->seek(start_position));
    }
    play_video_outputs(host);
    if (host.playback.audio->is_open()) {
        if (!host.playback.video_master_clock || host.playback.player.has_future_data()) {
            static_cast<void>(host.playback.audio->play());
            host.playback.video_buffering.store(false);
        } else {
            host.playback.video_buffering.store(true);
            host.playback.video_buffering_position.store(
                start_position, std::memory_order_relaxed);
        }
    }
    host.playback.position.store(start_position);
    host.playback.last_audio_sync = std::chrono::steady_clock::now();
}

bool handle_stage_device(ShowHost& host, void* native_d3d11_device) {
    auto* device = static_cast<ID3D11Device*>(native_d3d11_device);
    if (!device || has_ts_stage_output(host)) return false;
    if (host.native_stage.preferred_stage_device.Get() == device
            && SUCCEEDED(device->GetDeviceRemovedReason())) {
        return true;
    }

    reset_unified_stage_device_pipeline(host);
    host.native_stage.preferred_stage_device = device;
    return true;
}

bool publish_unified_native_stage_frame(
        ShowHost& host,
        void* video_texture,
        std::uint32_t video_width,
        std::uint32_t video_height,
        kirakara::show::win32::UnifiedStageVideoFrameIdentity video_identity,
        double stage_time,
        std::uint64_t generation,
        bool draw_program_overlays,
        bool flush_d3d_before_publish) {
    const auto profile = host.native_stage.unified_stage_renderer.profile();
    if (!profile.valid()) return false;
    if (!host.native_stage.native_stage_generation_valid
            || host.native_stage.native_stage_generation != generation) {
        host.native_stage.native_stage_generation = generation;
        host.native_stage.native_stage_next_frame_id = 0;
        host.native_stage.native_stage_generation_valid = true;
    }
    // Stage content is evaluated at 60 fps. A faster display repeats the
    // latest immutable frame during scanout; it must not cause extra video
    // decode or lyric evaluation merely to match a 120/144 Hz monitor.
    constexpr std::int64_t frame_duration_100ns = 10000000LL / 60LL;
    kirakara::show::win32::UnifiedStageRenderTimings render_timings{};
    const auto result = host.native_stage.unified_stage_renderer.render(
        kirakara::show::win32::UnifiedStageRenderRequest{
            .timing = kirakara::show::StageFrameTiming{
                generation,
                host.native_stage.native_stage_next_frame_id,
                static_cast<std::int64_t>(
                    std::max(0.0, stage_time) * 10000000.0),
                frame_duration_100ns,
            },
            .project_time = stage_time,
            .video_texture = video_texture,
            .video_width = video_width,
            .video_height = video_height,
            .video_identity = video_identity,
            .lyrics = draw_program_overlays ? &host.playback.document : nullptr,
            .config = draw_program_overlays ? &host.playback.config : nullptr,
            .overlays = &host.playback.stage_overlay_state,
            .draw_song_title = draw_program_overlays,
            .draw_lyrics = draw_program_overlays,
            .idle_color = kirakara::show::Color{
                0.0F, 0.0F, 0.0F, 1.0F},
            .flush_d3d_before_publish = flush_d3d_before_publish,
            .diagnostics = host.native_stage.pipeline_diagnostics.enabled()
                ? &render_timings : nullptr,
        });
    record_stage_render_timings(
        host.native_stage.pipeline_diagnostics, render_timings);
    if (result
            != kirakara::show::win32::UnifiedStageRenderResult::published) {
        if (result == kirakara::show::win32::
                UnifiedStageRenderResult::dropped_no_free_frame) {
            host.native_stage.native_stage_dropped_frames.fetch_add(
                1, std::memory_order_relaxed);
            host.native_stage.pipeline_diagnostics.increment(
                NativeStagePipelineCounter::pool_drops);
        }
        return false;
    }
    host.native_stage.native_stage_produced_frames.fetch_add(
        1, std::memory_order_relaxed);
    host.native_stage.pipeline_diagnostics.increment(
        NativeStagePipelineCounter::published_frames);
    trace_native_stage_publish(
        generation, host.native_stage.native_stage_next_frame_id);
    ++host.native_stage.native_stage_next_frame_id;
    auto latest = host.native_stage.native_stage_mailbox.take_latest();
    if (!latest) return false;
    if (has_native_stage_window_output(host)) {
        host.native_stage.native_last_stage_frame = std::move(latest);
    } else {
        // Flutter owns the only visible Sink in controller-only mode. Do not
        // retain a second lease that forces the pool to rotate DXGI handles.
        host.native_stage.native_last_stage_frame = {};
    }
    // Presentation now owns a published frame: video frames keep the last
    // legal frame available, black/idle frames invalidate it.
    host.native_stage.native_presentation_holds_frame = video_texture != nullptr;
    return true;
}

void render_unified_native_stage(ShowHost& host) {
    const bool output_visible = has_physical_stage_output(host)
        || has_active_stage_texture_source(host);
    if (!output_visible || has_ts_stage_output(host)
            || !configure_unified_native_stage(host)) {
        return;
    }
    NativeStagePipelineDurationScope stage_tick_timing(
        host.native_stage.pipeline_diagnostics,
        NativeStagePipelineEvent::stage_tick);
    host.native_stage.pipeline_diagnostics.increment(
        NativeStagePipelineCounter::render_ticks);
    const bool native_window_output = has_native_stage_window_output(host);
    const bool flush_d3d_before_publish =
        has_active_external_texture_source(host) && !native_window_output;
    service_native_stage_standby_video(host);

    const auto state = host.playback.state.load();
    const bool playing = state == SHOW_STATE_PLAYING;
    const bool buffering = host.playback.video_buffering.load();
    double stage_time = host.playback.position.load();
    if (host.native_stage.native_video_start_pending) {
        stage_time = host.native_stage.native_video_start_position;
    } else if (playing) {
        stage_time = playback_time(host);
    } else if (state == SHOW_STATE_PAUSED && host.playback.has_paused_position) {
        stage_time = host.playback.paused_position;
    }

    const auto generation =
        host.playback.video_timeline_revision.load(std::memory_order_relaxed);
    const bool has_video = host.playback.current_media.loaded
        && !host.playback.current_media.video.empty();
    // 换歌过渡期（STOPPED/IDLE 且 transition holding）预打开新源：decoder
    // worker 提前 session.open/seek/解码，使按播放时首帧即出，避免"卡在
    // 旧帧几百 ms"。预打开帧不发布（等真正播放/start_pending 才显示），
    // 防止未播放就切换画面或误清 transition hold。
    const bool transition_preopen = has_video
        && host.output_transition.native_transition_holding
        && !host.native_stage.native_video_start_pending
        && (state == SHOW_STATE_STOPPED || state == SHOW_STATE_IDLE);
    auto decoder_state = MfD3D11PlaybackState::idle;
    if (has_video && host.native_stage.native_video_start_pending) {
        decoder_state = MfD3D11PlaybackState::paused;
    } else if (has_video && playing && !buffering) {
        decoder_state = MfD3D11PlaybackState::playing;
    } else if (has_video
            && (state == SHOW_STATE_PAUSED || buffering)) {
        decoder_state = MfD3D11PlaybackState::paused;
    } else if (transition_preopen) {
        decoder_state = MfD3D11PlaybackState::paused;
    }
    host.native_stage.stage_video_decoder.synchronize(
        has_video ? host.playback.current_media.video : std::wstring{},
        generation,
        stage_time,
        decoder_state);

    std::shared_ptr<const DecodedD3D11VideoFrame> decoded;
    if (decoder_state != MfD3D11PlaybackState::idle) {
        host.native_stage.pipeline_diagnostics.increment(
            NativeStagePipelineCounter::decoder_requests);
        {
            NativeStagePipelineDurationScope decoder_timing(
                host.native_stage.pipeline_diagnostics,
                NativeStagePipelineEvent::decoder_request);
            decoded = host.native_stage.stage_video_decoder.texture_for(
                host.playback.current_media.video, generation, stage_time);
        }
        if (decoded && !video_frame_matches_program_time(
                decoded->timestamp_seconds(), stage_time)) {
            decoded.reset();
        }
        if (decoded) {
            host.native_stage.pipeline_diagnostics.increment(
                NativeStagePipelineCounter::decoder_hits);
            if (host.native_stage.pipeline_diagnostics.enabled()) {
                const bool repeated =
                    host.native_stage.decoded_frame_identity_valid
                    && host.native_stage.decoded_frame_generation
                        == decoded->generation()
                    && host.native_stage.decoded_frame_id
                        == decoded->frame_id();
                host.native_stage.pipeline_diagnostics.increment(repeated
                    ? NativeStagePipelineCounter::repeated_video_frames
                    : NativeStagePipelineCounter::unique_video_frames);
                host.native_stage.decoded_frame_identity_valid = true;
                host.native_stage.decoded_frame_generation =
                    decoded->generation();
                host.native_stage.decoded_frame_id = decoded->frame_id();
            }
        } else {
            host.native_stage.pipeline_diagnostics.increment(
                NativeStagePipelineCounter::decoder_misses);
        }
    }

    auto texture_delivery = stage_texture_delivery_state(host);
    const auto retain_complete_output = [&] {
        // Flip-model swap chains and Flutter external textures retain their
        // most recently committed pixels. Only drain a frame already waiting
        // for the Preview sink; do not copy/present the same Stage at 60 Hz
        // merely to keep a static picture visible.
        if (native_window_output && texture_delivery.has_pending_frame) {
            notify_stage_texture_sources(host, true);
        }
        host.native_stage.pipeline_diagnostics.increment(
            NativeStagePipelineCounter::static_frame_skips);
    };

    if (transition_preopen) {
        retain_complete_output();
        return;
    }

    // Once a real frame for the current program has aired, a transient
    // decoder miss must not publish a black replacement. This is common
    // during the first Media Foundation samples of a cold load and was
    // visible as a one-frame black flash in the Flutter texture. The explicit
    // stop/end/load paths clear readiness when black is actually required.
    const bool active_program_needs_last_frame = has_video
        && (host.native_stage.stage_program_ready
            || has_current_native_video_program(host))
        && (playing || state == SHOW_STATE_PAUSED || buffering);
    const bool hold_complete_program = !decoded
        && host.native_stage.native_presentation_holds_frame
        && (host.output_transition.native_transition_holding
            || active_program_needs_last_frame);
    if (hold_complete_program) {
        retain_complete_output();
        return;
    }

    const auto profile = host.native_stage.unified_stage_renderer.profile();
    const bool clock_advancing = playing && !buffering
        && !host.native_stage.native_video_start_pending;
    const bool waiting_for_static_video = has_video && !decoded
        && decoder_state != MfD3D11PlaybackState::idle;
    const NativeStageContentIdentity content_identity{
        .static_content = !clock_advancing && !waiting_for_static_video,
        .has_video_frame = decoded != nullptr,
        .draw_program_overlays = decoded != nullptr,
        .native_video_start_pending =
            host.native_stage.native_video_start_pending,
        .physical_output = native_window_output,
        .playback_state = state,
        .program_generation = generation,
        .decoded_generation = decoded ? decoded->generation() : 0,
        .decoded_frame_id = decoded ? decoded->frame_id() : 0,
        .source_width = decoded ? decoded->width() : 0,
        .source_height = decoded ? decoded->height() : 0,
        .position_100ns = static_cast<std::int64_t>(std::llround(
            std::max(0.0, stage_time) * 10000000.0)),
        .playback_revision = host.playback.playback_revision.load(
            std::memory_order_relaxed),
        .overlay_revision = host.playback.stage_overlay_revision.load(
            std::memory_order_acquire),
        .output_revision = native_window_output
            ? host.native_stage.unified_stage_presenter.output_revision() : 0,
        .output_width = profile.width,
        .output_height = profile.height,
    };
    const bool force_redraw = host.native_stage.force_stage_redraw
        || host.native_stage.native_output_recovery_pending
        || texture_delivery.requires_new_stage_frame;
    if (!host.native_stage.frame_scheduler.should_render(
            content_identity, force_redraw)) {
        retain_complete_output();
        return;
    }

    bool delivered_current_video_frame = false;
    if (decoded) {
        if (!publish_unified_native_stage_frame(
                host,
                decoded->texture(),
                decoded->width(),
                decoded->height(),
                kirakara::show::win32::UnifiedStageVideoFrameIdentity{
                    .valid = true,
                    .generation = decoded->generation(),
                    .frame_id = decoded->frame_id(),
                    .width = decoded->width(),
                    .height = decoded->height(),
                },
                stage_time,
                generation,
                true,
                flush_d3d_before_publish)) {
            return;
        }
        delivered_current_video_frame = true;
    } else {
        if (!publish_unified_native_stage_frame(
                host,
                nullptr,
                0,
                0,
                {},
                stage_time,
                generation,
                false,
                flush_d3d_before_publish)) {
            return;
        }
    }

    if (native_window_output) {
        const auto finish_present = [&] {
            host.native_stage.native_stage_presented_frames.fetch_add(
                1, std::memory_order_relaxed);
            notify_stage_texture_sources(host, true);
            host.native_stage.frame_scheduler.commit(content_identity);
            host.native_stage.force_stage_redraw = false;
            if (delivered_current_video_frame) {
                record_native_first_frame_if_pending(host);
                host.native_stage.stage_video_frame_available = true;
                mark_current_native_video_program_ready(host, generation);
                static_cast<void>(refresh_stage_program_ready(host));
                host.output_transition.native_transition_holding = false;
                host.output_transition.transition_hold_started = {};
                release_native_video_start_gate(host);
            }
        };
        const auto present_frame = [&host]() {
            if (!host.native_stage.native_last_stage_frame) return false;
            NativeStagePipelineDurationScope present_timing(
                host.native_stage.pipeline_diagnostics,
                NativeStagePipelineEvent::physical_present);
            const bool presented =
                host.native_stage.unified_stage_presenter.present(
                    host.native_stage.native_last_stage_frame, false);
            if (presented) {
                host.native_stage.pipeline_diagnostics.increment(
                    NativeStagePipelineCounter::physical_presents);
            }
            return presented;
        };
        if (present_frame()) {
            finish_present();
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const bool log_present_error =
            host.native_stage.native_present_error_log_due.time_since_epoch().count() == 0
            || now >= host.native_stage.native_present_error_log_due;
        if (log_present_error) {
            host.native_stage.native_present_error_log_due = now + std::chrono::seconds(1);
            log_hresult("Unified native Stage present failed",
                static_cast<HRESULT>(
                    host.native_stage.unified_stage_presenter.last_error_code()));
        }
        auto* device = static_cast<ID3D11Device*>(
            host.native_stage.unified_stage_renderer.native_d3d_device());
        if (device && FAILED(device->GetDeviceRemovedReason())) {
            log_line("Unified native Stage device removed; rebuilding");
            begin_native_output_recovery(host);
            reset_unified_stage_device_pipeline(host, true);
        } else if (host.native_stage.unified_stage_presenter.recover_output()
                && present_frame()) {
            finish_present();
        } else {
            if (log_present_error) {
                log_hresult("Unified native Stage output recovery failed",
                    static_cast<HRESULT>(
                        host.native_stage.unified_stage_presenter.last_error_code()));
            }
            begin_native_output_recovery(host);
            host.native_stage.unified_stage_presenter.shutdown();
        }
        return;
    }
    notify_stage_texture_sources(host, false);
    host.native_stage.frame_scheduler.commit(content_identity);
    host.native_stage.force_stage_redraw = false;
    if (delivered_current_video_frame) {
        record_native_first_frame_if_pending(host);
        host.native_stage.stage_video_frame_available = true;
        mark_current_native_video_program_ready(host, generation);
        static_cast<void>(refresh_stage_program_ready(host));
        host.output_transition.native_transition_holding = false;
        host.output_transition.transition_hold_started = {};
        release_native_video_start_gate(host);
    }
}

bool has_current_native_video_program(const ShowHost& host) {
    return host.native_stage.native_video_program_ready
        && host.native_stage.native_last_stage_frame
        && host.native_stage.native_video_ready_timeline_revision
            == host.playback.video_timeline_revision.load(std::memory_order_relaxed);
}

void mark_current_native_video_program_ready(
        ShowHost& host, std::uint64_t timeline_revision) {
    host.native_stage.native_video_program_ready = true;
    host.native_stage.native_video_ready_timeline_revision = timeline_revision;
}

void clear_current_native_video_program(ShowHost& host) {
    host.native_stage.native_video_program_ready = false;
    host.native_stage.native_video_ready_timeline_revision = 0;
}
