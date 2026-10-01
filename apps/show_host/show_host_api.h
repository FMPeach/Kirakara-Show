#pragma once

#ifdef SHOW_HOST_EXPORTS
#define SHOW_HOST_API __declspec(dllexport)
#else
#define SHOW_HOST_API __declspec(dllimport)
#endif

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* ShowHostHandle;
typedef void* ShowHostStageTextureSource;
typedef void* ShowHostStageVisualSource;

typedef void (*ShowHostStageTextureFrameAvailableCallback)(
    void* user_data,
    uintptr_t shared_handle,
    uint32_t width,
    uint32_t height,
    uint64_t generation,
    uint64_t frame_id);

typedef struct ShowHostStageTextureFrame {
    uintptr_t shared_handle;
    uintptr_t lease_token;  // Reserved for ABI compatibility; currently zero.
    uint32_t width;
    uint32_t height;
    uint64_t generation;
    uint64_t frame_id;
} ShowHostStageTextureFrame;

// Versioned, opt-in transport for the DirectComposition Stage Visual. This is
// deliberately separate from ShowHostStageTextureSource so the stock Flutter
// external-texture fallback keeps its existing ABI and synchronization model.
enum {
    SHOW_HOST_STAGE_VISUAL_ABI_VERSION = 1,
    SHOW_HOST_STAGE_VISUAL_SYNC_KEYED_MUTEX = 1,
    SHOW_HOST_STAGE_VISUAL_FRAME_RESOURCE_CHANGED = 1 << 0,
    SHOW_HOST_STAGE_VISUAL_CAP_NT_HANDLE = 1 << 0,
    SHOW_HOST_STAGE_VISUAL_CAP_KEYED_MUTEX = 1 << 1,
    SHOW_HOST_STAGE_VISUAL_CAP_LATEST_FRAME = 1 << 2,
    SHOW_HOST_STAGE_VISUAL_CAP_NONBLOCKING_PRODUCER = 1 << 3,
};

typedef enum ShowHostStageVisualResult {
    SHOW_HOST_STAGE_VISUAL_SUCCESS = 0,
    SHOW_HOST_STAGE_VISUAL_INVALID_ARGUMENT = 1,
    SHOW_HOST_STAGE_VISUAL_VERSION_MISMATCH = 2,
    SHOW_HOST_STAGE_VISUAL_UNAVAILABLE = 3,
} ShowHostStageVisualResult;

typedef struct ShowHostStageVisualFrame {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t flags;
    uint32_t sync_type;
    // Borrowed NT handle. It remains valid only for the duration of the
    // callback. A consumer that queues work must DuplicateHandle before
    // returning; it must never close this borrowed value.
    uintptr_t shared_nt_handle;
    uint32_t width;
    uint32_t height;
    uint32_t dxgi_format;
    uint32_t reserved0;
    uint64_t resource_generation;
    uint64_t content_generation;
    uint64_t frame_id;
    uint64_t consumer_acquire_key;
    uint64_t consumer_release_key;
    uint64_t reserved[4];
} ShowHostStageVisualFrame;

// Called on Show's Stage producer thread after it has released keyed-mutex
// ownership to consumer_acquire_key. The callback must be bounded and must not
// call Dart, Flutter rendering, DirectComposition, wait for GPU work, or
// re-enter this source's control API. Clearing the callback, deactivating the
// source, and destroying it wait for an already-entered callback to return.
typedef void (*ShowHostStageVisualFrameAvailableCallback)(
    void* user_data,
    const ShowHostStageVisualFrame* frame);

typedef struct ShowHostStageVisualStats {
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t last_hresult;
    uint32_t reserved0;
    uint64_t published_frames;
    uint64_t producer_busy_drops;
    uint64_t callback_count;
    uint64_t resource_recreations;
    uint64_t resource_generation;
    uint64_t content_generation;
    uint64_t frame_id;
    uint64_t reserved[4];
} ShowHostStageVisualStats;

typedef int32_t (*ShowHostStageVisualCreateSource)(
    ShowHostHandle host,
    ShowHostStageVisualSource* source);
typedef void (*ShowHostStageVisualDestroySource)(
    ShowHostStageVisualSource source);
typedef void (*ShowHostStageVisualSetActive)(
    ShowHostStageVisualSource source,
    bool active);
typedef void (*ShowHostStageVisualSetFrameCallback)(
    ShowHostStageVisualSource source,
    ShowHostStageVisualFrameAvailableCallback callback,
    void* user_data);
typedef int32_t (*ShowHostStageVisualGetStats)(
    ShowHostStageVisualSource source,
    ShowHostStageVisualStats* stats);

typedef struct ShowHostStageVisualApi {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t capabilities;
    const char* protocol_revision;
    ShowHostStageVisualCreateSource create_source;
    ShowHostStageVisualDestroySource destroy_source;
    ShowHostStageVisualSetActive set_active;
    ShowHostStageVisualSetFrameCallback set_frame_callback;
    ShowHostStageVisualGetStats get_stats;
    uint64_t reserved[4];
} ShowHostStageVisualApi;

typedef struct ShowHostStageFrameStats {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t produced_frames;
    uint64_t presented_frames;
    uint64_t dropped_frames;
} ShowHostStageFrameStats;

enum {
    SHOW_CAST_EVENT_DECODER_REQUEST = 0,
    SHOW_CAST_EVENT_DECODER_SAMPLE_READY = 1,
    SHOW_CAST_EVENT_NORMALIZE = 2,
    SHOW_CAST_EVENT_OVERLAY_DRAW = 3,
    SHOW_CAST_EVENT_NV12_COMPOSE = 4,
    SHOW_CAST_EVENT_GPU_COPY = 5,
    SHOW_CAST_EVENT_ENCODER_PROCESS_INPUT = 6,
    SHOW_CAST_EVENT_ENCODER_OUTPUT = 7,
    SHOW_CAST_EVENT_TS_APPEND = 8,
    SHOW_CAST_EVENT_HTTP_FIRST_BYTE = 9,
    SHOW_CAST_EVENT_HTTP_SEND = 10,
    SHOW_CAST_EVENT_UNDERFLOW = 11,
    SHOW_CAST_EVENT_RECOVERED = 12,
    SHOW_CAST_EVENT_COUNT = 13,
};

// Bit flags in ShowHostCastPipelineTraceRecord.value for
// SHOW_CAST_EVENT_ENCODER_PROCESS_INPUT. Encoder-output trace values use a
// negative byte count for keyframes and a positive count for delta frames.
enum {
    SHOW_CAST_ENCODER_INPUT_DISCONTINUITY = 1 << 0,
    SHOW_CAST_ENCODER_INPUT_KEYFRAME_REQUESTED = 1 << 1,
    SHOW_CAST_ENCODER_INPUT_KEYFRAME_APPLIED = 1 << 2,
    SHOW_CAST_ENCODER_INPUT_KEYFRAME_UNSUPPORTED = 1 << 3,
    SHOW_CAST_ENCODER_INPUT_KEYFRAME_FAILED = 1 << 4,
};

typedef struct ShowHostCastPipelineEventStats {
    uint64_t count;
    uint64_t last_frame_id;
    uint64_t last_qpc;
    uint64_t maximum_gap_qpc;
    uint64_t total_duration_qpc;
    uint64_t maximum_duration_qpc;
} ShowHostCastPipelineEventStats;

// Versioned, query-only Cast diagnostics. QPC values are raw ticks; divide by
// qpc_frequency for seconds. This is a new ABI surface and does not extend any
// pre-existing structure.
typedef struct ShowHostCastPipelineStats {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t session_generation;
    uint64_t qpc_frequency;
    uint64_t decoded_frames;
    uint64_t repeated_frames;
    uint64_t video_processor_passes;
    uint64_t compute_dispatches;
    uint64_t gpu_copies;
    uint64_t explicit_flushes;
    uint64_t encoder_drops;
    uint64_t decoder_underflows;
    uint64_t decoder_recoveries;
    uint64_t client_rebases;
    uint64_t texture_creations;
    uint64_t view_creations;
    uint64_t missed_output_slots;
    ShowHostCastPipelineEventStats events[SHOW_CAST_EVENT_COUNT];
} ShowHostCastPipelineStats;

// Optional detailed event trace. It is disabled by default and remains fully
// in memory; callers may read it after stopping Cast and persist it elsewhere.
typedef struct ShowHostCastPipelineTraceRecord {
    uint64_t sequence;
    uint64_t session_generation;
    uint64_t qpc;
    uint64_t duration_qpc;
    uint64_t correlation_id;
    uint64_t related_id;
    int64_t value;
    uint32_t event;
    uint32_t thread_id;
} ShowHostCastPipelineTraceRecord;

typedef struct ShowHostCastPipelineTraceState {
    uint32_t struct_size;
    uint32_t reserved;
    uint32_t enabled;
    uint32_t capacity;
    uint64_t latest_sequence;
    uint64_t overwritten_events;
    uint64_t dropped_events;
} ShowHostCastPipelineTraceState;

enum {
    SHOW_CAST_BACKEND_COMPATIBILITY_C = 0,
    SHOW_CAST_BACKEND_NATIVE_NV12_VP_B = 1,
    SHOW_CAST_BACKEND_COMPUTE_NV12_A = 2,
};

enum {
    SHOW_CAST_BACKEND_REASON_COMPUTE_PATH_COMPLETE = 0,
    SHOW_CAST_BACKEND_REASON_COMPUTE_ENCODER_COPY_REQUIRED = 1,
    SHOW_CAST_BACKEND_REASON_PLANE_VIEWS_UNAVAILABLE = 2,
    SHOW_CAST_BACKEND_REASON_COMPUTE_BIND_UNAVAILABLE = 3,
    SHOW_CAST_BACKEND_REASON_NATIVE_NV12_UNAVAILABLE = 4,
    SHOW_CAST_BACKEND_REASON_DXGI_ENCODER_UNAVAILABLE = 5,
    SHOW_CAST_BACKEND_REASON_ENCODER_SAMPLE_RETENTION_UNAVAILABLE = 6,
    SHOW_CAST_BACKEND_REASON_VIDEO_PROCESSOR_UNAVAILABLE = 7,
    SHOW_CAST_BACKEND_REASON_PROBE_INCOMPLETE = 8,
    SHOW_CAST_BACKEND_REASON_INVALID_DEVICE = 9,
};

enum {
    SHOW_CAST_KEYFRAME_CONTROL_UNKNOWN = 0,
    SHOW_CAST_KEYFRAME_CONTROL_UNSUPPORTED = 1,
    SHOW_CAST_KEYFRAME_CONTROL_SUPPORTED = 2,
    SHOW_CAST_KEYFRAME_CONTROL_FAILED = 3,
};

typedef struct ShowHostCastCapabilityCheck {
    int32_t hresult;
    bool attempted;
    bool supported;
    uint8_t reserved[2];
} ShowHostCastCapabilityCheck;

typedef struct ShowHostCastCapabilityReport {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t adapter_luid;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t feature_level;
    uint32_t windows_build;
    uint32_t backend;
    uint32_t backend_reason;
    bool requires_encoder_copy;
    bool report_valid;
    bool encoder_retains_samples_async;
    bool backend_active;
    uint8_t force_keyframe_control;
    uint8_t reserved_flags[3];
    ShowHostCastCapabilityCheck device_healthy;
    ShowHostCastCapabilityCheck native_nv12_source;
    ShowHostCastCapabilityCheck nv12_plane_srvs;
    ShowHostCastCapabilityCheck nv12_plane_uavs;
    ShowHostCastCapabilityCheck uav_video_encoder_bind;
    ShowHostCastCapabilityCheck combined_uav_encoder_input;
    ShowHostCastCapabilityCheck compose_to_encoder_copy;
    ShowHostCastCapabilityCheck nv12_video_processor;
    ShowHostCastCapabilityCheck dxgi_h264_encoder;
    ShowHostCastCapabilityCheck tracked_sample_release;
    ShowHostCastCapabilityCheck even_420_geometry;
} ShowHostCastCapabilityReport;

typedef struct ShowHostStageOverlayAsset {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t content_revision;
    const wchar_t* cache_key;
    const wchar_t* local_path;
} ShowHostStageOverlayAsset;

typedef struct ShowHostStageOverlayState {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t revision;
    const wchar_t* qr_payload;
    ShowHostStageOverlayAsset qr_decoration;
    const wchar_t* announcement_text;
    ShowHostStageOverlayAsset announcement_decoration;
} ShowHostStageOverlayState;

enum {
    SHOW_STAGE_OVERLAY_QR_VISIBLE = 1U << 0,
    SHOW_STAGE_OVERLAY_ANNOUNCEMENT_VISIBLE = 1U << 1,
};

enum {
    SHOW_STATE_IDLE = 0,
    SHOW_STATE_PLAYING = 1,
    SHOW_STATE_PAUSED = 2,
    SHOW_STATE_STOPPED = 3,
};

enum {
    SHOW_AUDIO_TRACK_VOCAL = 0,
    SHOW_AUDIO_TRACK_ACCOMPANIMENT = 1,
};

enum {
    SHOW_CLOCK_AUDIO_MASTER = 0,
    SHOW_CLOCK_VIDEO_MASTER = 1,
};

enum {
    SHOW_TRANSITION_HARD = 0,
    SHOW_TRANSITION_SEAMLESS = 1,
};

/// Create an engine instance. Returns NULL on failure.
SHOW_HOST_API ShowHostHandle show_host_create(void);

/// Destroy and clean up the instance.
SHOW_HOST_API void show_host_destroy(ShowHostHandle handle);

/// Load media files. Both paths are UTF-16 (wchar_t*) on Windows.
/// Returns false if files can't be opened.
SHOW_HOST_API bool show_host_load(ShowHostHandle handle,
                                  const wchar_t* video_path,
                                  const wchar_t* lyric_path,
                                  const wchar_t* vocal_path,
                                  const wchar_t* accompaniment_path);

/// Load media with an explicit playback clock. Video-master mode is intended
/// for external DASH media where the separately processed audio follows video.
SHOW_HOST_API bool show_host_load_with_clock(
    ShowHostHandle handle,
    const wchar_t* video_path,
    const wchar_t* lyric_path,
    const wchar_t* vocal_path,
    const wchar_t* accompaniment_path,
    int clock_mode);

/// Load media with explicit clock and transition behavior. Seamless mode keeps
/// the last complete Stage frame visible until the new program can take over.
SHOW_HOST_API bool show_host_load_with_options(
    ShowHostHandle handle,
    const wchar_t* video_path,
    const wchar_t* lyric_path,
    const wchar_t* vocal_path,
    const wchar_t* accompaniment_path,
    int clock_mode,
    int transition_mode);

/// Prepare the next song without changing the active playback session.
/// KRL parsing and processed audio decoding run on a background worker; a
/// matching later load consumes the prepared result atomically. While a
/// Native Stage/Preview lease is active, the host also retains one bounded
/// decoded video first frame for a matching load; Cast remains independent.
SHOW_HOST_API bool show_host_prepare_next(
    ShowHostHandle handle,
    const wchar_t* video_path,
    const wchar_t* lyric_path,
    const wchar_t* vocal_path,
    const wchar_t* accompaniment_path,
    int clock_mode);

/// Playback control.
SHOW_HOST_API void show_host_play(ShowHostHandle handle);
SHOW_HOST_API void show_host_pause(ShowHostHandle handle);
SHOW_HOST_API void show_host_stop(ShowHostHandle handle);
SHOW_HOST_API void show_host_seek(ShowHostHandle handle, double seconds);
SHOW_HOST_API void show_host_set_volume(ShowHostHandle handle,
                                        int32_t volume_percent);
SHOW_HOST_API void show_host_set_key_semitones(ShowHostHandle handle,
                                               int32_t key_semitones);
SHOW_HOST_API void show_host_set_audio_clock_offset(ShowHostHandle handle,
                                                    double seconds);
SHOW_HOST_API bool show_host_set_audio_track(ShowHostHandle handle,
                                             int track);

/// Show or hide the native Stage Output window.
SHOW_HOST_API void show_host_set_stage_visible(ShowHostHandle handle,
                                               bool visible);

/// Place the native Stage Output window in physical desktop pixels.
SHOW_HOST_API void show_host_set_stage_window_rect(ShowHostHandle handle,
                                                   int32_t x,
                                                   int32_t y,
                                                   uint32_t width,
                                                   uint32_t height);

/// Select the D3D11 device used by Unified Stage. Flutter supplies a device
/// created on its rendering adapter before attaching the texture source so
/// DXGI shared handles remain valid on hybrid-GPU systems.
SHOW_HOST_API bool show_host_set_stage_d3d_device(
    ShowHostHandle handle,
    uintptr_t native_d3d11_device);

/// Create a non-blocking latest-frame source for a Flutter GPU texture. The
/// source has an independent lifetime and becomes inert when its ShowHost is
/// destroyed. Its shared handle stays stable until the Stage output profile
/// or D3D11 device changes; frame contents advance in place.
SHOW_HOST_API ShowHostStageTextureSource
show_host_create_stage_texture_source(ShowHostHandle handle);
SHOW_HOST_API void show_host_destroy_stage_texture_source(
    ShowHostStageTextureSource source);
SHOW_HOST_API void show_host_stage_texture_source_set_active(
    ShowHostStageTextureSource source,
    bool active);
SHOW_HOST_API void show_host_stage_texture_source_set_frame_callback(
    ShowHostStageTextureSource source,
    ShowHostStageTextureFrameAvailableCallback callback,
    void* user_data);
SHOW_HOST_API bool show_host_stage_texture_source_acquire(
    ShowHostStageTextureSource source,
    ShowHostStageTextureFrame* frame);
SHOW_HOST_API void show_host_release_stage_texture_frame(
    uintptr_t lease_token);
/// Resolve the versioned non-blocking NT-handle/keyed-mutex transport used by
/// the DirectComposition Stage Visual. A mismatched App must fail explicitly;
/// it must not reinterpret a different table layout.
SHOW_HOST_API int32_t show_host_get_stage_visual_api(
    uint32_t requested_abi_version,
    ShowHostStageVisualApi* api);
SHOW_HOST_API bool show_host_get_stage_frame_stats(
    ShowHostHandle handle,
    ShowHostStageFrameStats* stats);
SHOW_HOST_API bool show_host_get_cast_pipeline_stats(
    ShowHostHandle handle,
    ShowHostCastPipelineStats* stats);
SHOW_HOST_API bool show_host_set_cast_pipeline_trace_enabled(
    ShowHostHandle handle,
    bool enabled);
SHOW_HOST_API bool show_host_get_cast_pipeline_trace_state(
    ShowHostHandle handle,
    ShowHostCastPipelineTraceState* state);
SHOW_HOST_API uint32_t show_host_read_cast_pipeline_trace(
    ShowHostHandle handle,
    ShowHostCastPipelineTraceRecord* records,
    uint32_t capacity,
    uint64_t after_sequence);
SHOW_HOST_API bool show_host_get_cast_capability_report(
    ShowHostHandle handle,
    ShowHostCastCapabilityReport* report);
/// Diagnostic count of video-source owners in the active Cast topology.
/// A loaded Cast program must report exactly one: the Cast SourceReader.
/// After Cast stops, the restored MediaEngine is the single owner instead.
SHOW_HOST_API uint32_t show_host_get_active_video_decoder_owners(
    ShowHostHandle handle);

/// Replace the audience-facing overlay state. Phase 6 stores and transports
/// this state but deliberately renders no temporary QR or announcement art.
/// Asset paths point to App-owned cache files and are copied synchronously.
SHOW_HOST_API bool show_host_set_stage_overlay_state(
    ShowHostHandle handle,
    const ShowHostStageOverlayState* state);
SHOW_HOST_API uint64_t show_host_get_stage_overlay_revision(
    ShowHostHandle handle);

/// Query playback time (in seconds).
SHOW_HOST_API double show_host_get_position(ShowHostHandle handle);
SHOW_HOST_API double show_host_get_duration(ShowHostHandle handle);
SHOW_HOST_API int show_host_get_state(ShowHostHandle handle);
SHOW_HOST_API bool show_host_is_buffering(ShowHostHandle handle);

/// Query render surface dimensions.
SHOW_HOST_API uint32_t show_host_get_width(ShowHostHandle handle);
SHOW_HOST_API uint32_t show_host_get_height(ShowHostHandle handle);

/// Start the native MPEG-TS Stage stream. Hardware H.264 consumes the composed
/// D3D11 texture directly; the system software fallback reads back only the
/// converted NV12 frame. Neither path captures an HWND. Pass 0 to select a
/// free HTTP port.
SHOW_HOST_API bool show_host_start_cast_stream(ShowHostHandle handle,
                                               uint16_t port);
/// Return the bound native Cast HTTP port, or 0 when Cast is not running.
SHOW_HOST_API uint16_t show_host_get_cast_stream_port(ShowHostHandle handle);
/// Disconnect the cast stream and stop pushing frames.
SHOW_HOST_API void show_host_stop_cast_stream(ShowHostHandle handle);

#ifdef __cplusplus
}
#endif
