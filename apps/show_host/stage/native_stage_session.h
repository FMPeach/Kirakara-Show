#pragma once

#include "../show_host_api.h"
#include "../media/mf_d3d11_video_decoder.h"
#include "native_stage_frame_scheduler.h"
#include "native_stage_pipeline_diagnostics.h"
#include "native_stage_preview_load_policy.h"
#include "native_stage_trace.h"
#include "native_stage_video_decoder_config.h"
#include "stage_texture_publication_cadence.h"

#include "kirakara/show/stage_frame.hpp"
#include "kirakara/show/win32/stage_window_presenter.hpp"
#include "kirakara/show/win32/unified_stage_renderer.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct ShowHost;

using StageTextureFrameAvailableCallback = void (*)(
    void* user_data,
    std::uintptr_t shared_handle,
    std::uint32_t width,
    std::uint32_t height,
    std::uint64_t generation,
    std::uint64_t frame_id);

enum class StagePreviewTransport : std::uint8_t {
    flutter_external_texture,
    dcomp_visual_keyed_mutex,
};

struct StageTextureSourceState {
    std::mutex mutex;
    StagePreviewTransport transport{StagePreviewTransport::flutter_external_texture};
    kirakara::show::StageFrameMailbox mailbox;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> publication_texture;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> publication_keyed_mutex;
    std::uintptr_t publication_shared_handle{};
    HANDLE publication_nt_handle{};
    kirakara::show::StageOutputProfile publication_profile;
    kirakara::show::StageFrameTiming publication_timing;
    std::uint64_t publication_resource_generation{};
    std::uint64_t next_resource_generation{1};
    std::uint64_t visual_notified_resource_generation{};
    bool publication_ready{};
    bool callback_notification_pending{};
    StageTextureFrameAvailableCallback callback{};
    ShowHostStageVisualFrameAvailableCallback visual_callback{};
    void* callback_context{};
    std::condition_variable visual_callback_condition;
    std::uint32_t visual_callbacks_in_flight{};
    StageTexturePublicationCadence publication_cadence;
    std::int32_t visual_last_hresult{S_OK};
    std::uint64_t visual_published_frames{};
    std::uint64_t visual_producer_busy_drops{};
    std::uint64_t visual_callback_count{};
    std::uint64_t visual_resource_recreations{};
    bool active{};
    bool alive{true};
};

// Owns every GPU resource and readiness flag used by the physical Stage and
// Flutter texture sinks. During the first extraction step the host still
// drives the free functions below; moving ownership here prevents Cast work
// from reaching into the native output implementation.
struct NativeStageSession {
    NativeStageTraceSession trace_session;
    NativeStagePipelineDiagnostics pipeline_diagnostics;
    Microsoft::WRL::ComPtr<ID3D11Device> preferred_stage_device;
    kirakara::show::win32::UnifiedStageRenderer unified_stage_renderer;
    kirakara::show::win32::StageWindowPresenter unified_stage_presenter;
    kirakara::show::StageFrameMailbox native_stage_mailbox;
    kirakara::show::StageFrameLease native_last_stage_frame;
    std::atomic<std::uint64_t> native_stage_produced_frames{};
    std::atomic<std::uint64_t> native_stage_presented_frames{};
    std::atomic<std::uint64_t> native_stage_dropped_frames{};
    std::mutex stage_texture_sources_mutex;
    std::vector<std::weak_ptr<StageTextureSourceState>> stage_texture_sources;
    MfD3D11VideoDecoder stage_video_decoder;
    bool stage_video_decoder_started{};
    // Bounded next-program slot. A paused directive asks MF for only the first
    // frame at t=0, then sleeps until atomically promoted. Keep normal active
    // queue capacity because the complete worker moves into the active slot.
    MfD3D11VideoDecoder standby_video_decoder;
    std::wstring standby_video_source;
    std::uint64_t standby_prepare_generation{};
    std::uint64_t standby_video_timeline_revision{};
    std::uint64_t standby_prepare_started_qpc{};
    bool standby_video_decoder_started{};
    bool standby_video_requested{};
    bool standby_video_ready{};

    bool stage_program_ready{};
    bool stage_video_frame_available{};
    bool native_video_program_ready{};
    std::uint64_t native_video_ready_timeline_revision{};
    bool native_video_start_pending{};
    double native_video_start_position{};
    std::chrono::steady_clock::time_point native_video_start_started{};
    std::uint64_t native_video_start_diagnostics_qpc{};
    bool native_output_recovery_pending{};
    std::chrono::steady_clock::time_point native_present_error_log_due{};
    bool native_presentation_holds_frame{};

    std::uint64_t native_stage_generation{};
    std::uint64_t native_stage_next_frame_id{};
    bool native_stage_generation_valid{};
    bool native_stage_configured{};
    NativeStageFrameScheduler frame_scheduler;
    NativeStagePreviewLoadPolicy preview_load_policy;
    bool force_stage_redraw{};

    bool decoded_frame_identity_valid{};
    std::uint64_t decoded_frame_generation{};
    std::uint64_t decoded_frame_id{};
};

bool publish_stage_texture_frame(
    StageTextureSourceState& source,
    const kirakara::show::StageFrame& frame,
    NativeStagePipelineDiagnostics& diagnostics);
void release_stage_preview_publication_resources(
    StageTextureSourceState& source) noexcept;
void notify_stage_texture_sources(
    ShowHost& host, bool physical_stage_active);
void invalidate_stage_texture_sources(ShowHost& host);
void rebind_stage_texture_sources(ShowHost& host);

bool replace_unified_stage_renderer(
    ShowHost& host, const kirakara::show::StageOutputProfile& profile);
void reset_unified_stage_device_pipeline(
    ShowHost& host, bool release_preferred_device = false);
bool has_native_stage_window_output(ShowHost& host);
bool same_stage_profile(
    const kirakara::show::StageOutputProfile& left,
    const kirakara::show::StageOutputProfile& right);
kirakara::show::StageOutputProfile desired_native_stage_profile(ShowHost& host);
void release_unified_native_stage_frames(ShowHost& host);
bool configure_unified_native_stage(ShowHost& host);
bool has_requested_native_stage_output(ShowHost& host);

bool prime_native_stage_video(ShowHost& host, double position);
void request_native_stage_standby_video(
    ShowHost& host,
    const std::wstring& source,
    std::uint64_t prepare_generation);
void suspend_native_stage_standby_video(
    ShowHost& host, bool preserve_request);
bool promote_native_stage_standby_video(
    ShowHost& host,
    const std::wstring& source,
    std::uint64_t prepare_generation,
    std::uint64_t timeline_revision);
void clear_native_video_start_gate(ShowHost& host);
void begin_native_output_recovery(ShowHost& host);
void release_native_video_start_gate(ShowHost& host);
bool handle_stage_device(ShowHost& host, void* native_d3d11_device);

bool publish_unified_native_stage_frame(
    ShowHost& host,
    void* video_texture,
    std::uint32_t video_width,
    std::uint32_t video_height,
    kirakara::show::win32::UnifiedStageVideoFrameIdentity video_identity,
    double stage_time,
    std::uint64_t generation,
    bool draw_program_overlays,
    bool flush_d3d_before_publish);

bool has_current_native_video_program(const ShowHost& host);
void mark_current_native_video_program_ready(
    ShowHost& host, std::uint64_t timeline_revision);
void clear_current_native_video_program(ShowHost& host);

void render_unified_native_stage(ShowHost& host);
