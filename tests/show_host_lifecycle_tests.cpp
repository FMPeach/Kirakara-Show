// Headless host-contract tests.
//
// These lock the observable C ABI behaviour that internal refactors must not
// change, and they deliberately need no media, no window and no GPU:
//
//   * the synchronous / asynchronous command split (SendMessage vs
//     PostMessage) and the message ordering that the App relies on;
//   * the playback state machine reachable through the public commands;
//   * the output-lease side effects: stopping Cast while it is not running
//     must be inert, and it must never rewind or re-open the native program;
//   * the single-video-owner invariant exposed for diagnostics;
//   * the struct_size / reserved validation of every query struct, which is
//     the runtime half of the ABI contract (the exported symbol set is
//     checked separately by tool/show_host_abi.ps1).
//
// Media-dependent behaviour (frame cadence, handoff, Cast transport) stays in
// kirakara_show_unified_native_smoke / _cast_smoke, which need real fixtures.

#include "../apps/show_host/show_host_api.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void milestone(const char* message) {
    std::cout << "[host-lifecycle] " << message << std::endl;
}

void note(const std::string& message) {
    std::cout << "[host-lifecycle] note: " << message << std::endl;
}

// play/pause/stop are posted, so their effect is not visible when the call
// returns. SendMessage is not a queue-ordering fence for already posted
// messages: Windows may dispatch it before an older PostMessage. Observe the
// public state with a bounded wait instead of encoding that false assumption
// in the host contract.
void wait_for_state(ShowHostHandle host, int expected, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (show_host_get_state(host) == expected) return;
        std::this_thread::sleep_for(1ms);
    }
    expect(false, message);
}

const wchar_t* const kMissingMedia = L"Z:\\kirakara\\missing-media-for-tests.mp4";

HWND find_current_process_window(const wchar_t* class_name) {
    struct Search {
        const wchar_t* class_name{};
        HWND result{};
    } search{class_name};
    EnumWindows([](HWND window, LPARAM context) {
        auto& search = *reinterpret_cast<Search*>(context);
        DWORD process_id{};
        GetWindowThreadProcessId(window, &process_id);
        if (process_id != GetCurrentProcessId()) return TRUE;
        wchar_t actual_class[128]{};
        if (GetClassNameW(window, actual_class,
                static_cast<int>(
                    sizeof(actual_class) / sizeof(actual_class[0]))) > 0
                && std::wstring_view(actual_class) == search.class_name) {
            search.result = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.result;
}

}  // namespace

int main() {
    milestone("create reports an idle host with no decoder owner");
    ShowHostHandle host = show_host_create();
    expect(host != nullptr, "show_host_create returned null");
    expect(show_host_get_state(host) == SHOW_STATE_IDLE,
        "a fresh host is not IDLE");
    expect(show_host_get_position(host) == 0.0,
        "a fresh host does not report position 0");
    expect(show_host_get_duration(host) == 0.0,
        "a fresh host does not report duration 0");
    expect(!show_host_is_buffering(host),
        "a fresh host reports buffering");
    expect(show_host_get_active_video_decoder_owners(host) == 0,
        "a fresh host claims a video decoder owner");
    expect(show_host_get_cast_stream_port(host) == 0,
        "a fresh host exposes a Cast port");

    milestone("physical Stage can insert black before the first program");
    show_host_set_stage_window_rect(host, -32000, -32000, 64, 36);
    show_host_set_stage_visible(host, true);
    const auto idle_stage = find_current_process_window(
        L"KirakaraShowHostStage");
    const auto idle_surface = idle_stage
        ? FindWindowExW(idle_stage, nullptr,
            L"KirakaraShowHostVideoBlank", nullptr)
        : nullptr;
    expect(idle_stage && idle_surface,
        "fresh host did not create its physical Stage HWNDs");
    expect(IsWindowVisible(idle_stage) && IsWindowVisible(idle_surface),
        "physical Stage stayed hidden until the first program load");
    expect(reinterpret_cast<HBRUSH>(GetClassLongPtrW(
                idle_stage, GCLP_HBRBACKGROUND))
                == GetStockObject(BLACK_BRUSH)
            && reinterpret_cast<HBRUSH>(GetClassLongPtrW(
                idle_surface, GCLP_HBRBACKGROUND))
                == GetStockObject(BLACK_BRUSH),
        "idle physical Stage HWNDs do not use a black background");
    show_host_set_stage_visible(host, false);
    expect(!IsWindowVisible(idle_stage) && !IsWindowVisible(idle_surface),
        "physical Stage did not hide after releasing its idle lease");

    milestone("versioned Stage Visual ABI rejects mismatches");
    expect(show_host_get_stage_visual_api(
            SHOW_HOST_STAGE_VISUAL_ABI_VERSION, nullptr)
            == SHOW_HOST_STAGE_VISUAL_INVALID_ARGUMENT,
        "Stage Visual ABI accepted a null table");
    ShowHostStageVisualApi visual_api{};
    visual_api.struct_size = sizeof(visual_api);
    expect(show_host_get_stage_visual_api(
            SHOW_HOST_STAGE_VISUAL_ABI_VERSION + 1, &visual_api)
            == SHOW_HOST_STAGE_VISUAL_VERSION_MISMATCH,
        "Stage Visual ABI accepted the wrong version");
    visual_api = {};
    visual_api.struct_size = sizeof(visual_api) - 1;
    expect(show_host_get_stage_visual_api(
            SHOW_HOST_STAGE_VISUAL_ABI_VERSION, &visual_api)
            == SHOW_HOST_STAGE_VISUAL_VERSION_MISMATCH,
        "Stage Visual ABI accepted the wrong table size");
    visual_api = {};
    visual_api.struct_size = sizeof(visual_api);
    expect(show_host_get_stage_visual_api(
            SHOW_HOST_STAGE_VISUAL_ABI_VERSION, &visual_api)
            == SHOW_HOST_STAGE_VISUAL_SUCCESS,
        "Stage Visual ABI rejected a matching table");
    constexpr std::uint64_t required_visual_capabilities =
        SHOW_HOST_STAGE_VISUAL_CAP_NT_HANDLE
        | SHOW_HOST_STAGE_VISUAL_CAP_KEYED_MUTEX
        | SHOW_HOST_STAGE_VISUAL_CAP_LATEST_FRAME
        | SHOW_HOST_STAGE_VISUAL_CAP_NONBLOCKING_PRODUCER;
    expect(visual_api.abi_version == SHOW_HOST_STAGE_VISUAL_ABI_VERSION
            && (visual_api.capabilities & required_visual_capabilities)
                == required_visual_capabilities
            && visual_api.protocol_revision
            && std::string_view(visual_api.protocol_revision)
                == "nt-keyed-latest-v1"
            && visual_api.create_source && visual_api.destroy_source
            && visual_api.set_active && visual_api.set_frame_callback
            && visual_api.get_stats,
        "Stage Visual ABI returned incomplete or unexpected metadata");
    ShowHostStageVisualSource visual_source{};
    expect(visual_api.create_source(host, &visual_source)
            == SHOW_HOST_STAGE_VISUAL_SUCCESS
            && visual_source,
        "Stage Visual source was not created");
    ShowHostStageVisualStats visual_stats{};
    visual_stats.struct_size = sizeof(visual_stats) - 1;
    visual_stats.abi_version = SHOW_HOST_STAGE_VISUAL_ABI_VERSION;
    expect(visual_api.get_stats(visual_source, &visual_stats)
            == SHOW_HOST_STAGE_VISUAL_VERSION_MISMATCH,
        "Stage Visual stats accepted the wrong struct size");
    visual_stats = {};
    visual_stats.struct_size = sizeof(visual_stats);
    visual_stats.abi_version = SHOW_HOST_STAGE_VISUAL_ABI_VERSION + 1;
    expect(visual_api.get_stats(visual_source, &visual_stats)
            == SHOW_HOST_STAGE_VISUAL_VERSION_MISMATCH,
        "Stage Visual stats accepted the wrong ABI version");
    visual_stats = {};
    visual_stats.struct_size = sizeof(visual_stats);
    visual_stats.abi_version = SHOW_HOST_STAGE_VISUAL_ABI_VERSION;
    expect(visual_api.get_stats(visual_source, &visual_stats)
            == SHOW_HOST_STAGE_VISUAL_SUCCESS
            && visual_stats.published_frames == 0
            && visual_stats.producer_busy_drops == 0,
        "fresh Stage Visual source returned invalid statistics");
    visual_api.destroy_source(visual_source);

    milestone("play, pause, resume and stop are posted commands");
    show_host_play(host);
    wait_for_state(host, SHOW_STATE_PLAYING,
        "play did not settle into PLAYING");

    show_host_pause(host);
    wait_for_state(host, SHOW_STATE_PAUSED,
        "pause did not settle into PAUSED");

    show_host_play(host);
    wait_for_state(host, SHOW_STATE_PLAYING,
        "resume did not settle back into PLAYING");

    show_host_stop(host);
    wait_for_state(host, SHOW_STATE_STOPPED,
        "stop did not settle into STOPPED");
    expect(show_host_get_position(host) == 0.0,
        "stop did not rewind the position to 0");

    milestone("synchronous commands take effect before they return");
    // handle_seek() stores the requested position verbatim: bounds belong to
    // the caller, and the host only clamps what it hands to the audio backend.
    // Lock that down, because adding an ABI-level clamp would silently change
    // what the App observes.
    show_host_seek(host, -5.0);
    expect(show_host_get_position(host) == -5.0,
        "seek no longer stores the requested position verbatim (negative)");
    show_host_seek(host, 1234.5);
    expect(show_host_get_position(host) == 1234.5,
        "seek no longer stores the requested position verbatim (positive)");

    milestone("stopping Cast without a lease is inert and idempotent");
    const auto state_before_idle_cast_stop = show_host_get_state(host);
    const auto position_before_idle_cast_stop = show_host_get_position(host);
    show_host_stop_cast_stream(host);
    show_host_stop_cast_stream(host);
    expect(show_host_get_state(host) == state_before_idle_cast_stop,
        "an inactive Cast stop changed the playback state");
    expect(show_host_get_position(host) == position_before_idle_cast_stop,
        "an inactive Cast stop rewound the program");
    expect(show_host_get_active_video_decoder_owners(host) == 0,
        "an inactive Cast stop created a decoder owner");

    milestone("audio track selection needs loaded tracks");
    expect(!show_host_set_audio_track(host, SHOW_AUDIO_TRACK_VOCAL),
        "vocal track was selected without a loaded source");
    expect(!show_host_set_audio_track(host, SHOW_AUDIO_TRACK_ACCOMPANIMENT),
        "accompaniment track was selected without a loaded source");

    milestone("query structs validate struct_size and reserved");
    ShowHostStageFrameStats frame_stats{};
    frame_stats.struct_size = 0;
    expect(!show_host_get_stage_frame_stats(host, &frame_stats),
        "stage frame stats accepted struct_size 0");
    frame_stats.struct_size = sizeof(frame_stats);
    frame_stats.reserved = 1;
    expect(!show_host_get_stage_frame_stats(host, &frame_stats),
        "stage frame stats accepted a non-zero reserved field");
    frame_stats.reserved = 0;
    expect(show_host_get_stage_frame_stats(host, &frame_stats),
        "stage frame stats rejected a well-formed struct");

    ShowHostCastPipelineStats cast_stats{};
    cast_stats.struct_size = 0;
    expect(!show_host_get_cast_pipeline_stats(host, &cast_stats),
        "cast pipeline stats accepted struct_size 0");
    cast_stats.struct_size = sizeof(cast_stats);
    expect(show_host_get_cast_pipeline_stats(host, &cast_stats),
        "cast pipeline stats rejected a well-formed struct");

    ShowHostCastPipelineTraceState trace_state{};
    trace_state.struct_size = 0;
    expect(!show_host_get_cast_pipeline_trace_state(host, &trace_state),
        "cast trace state accepted struct_size 0");
    trace_state.struct_size = sizeof(trace_state);
    expect(show_host_get_cast_pipeline_trace_state(host, &trace_state),
        "cast trace state rejected a well-formed struct");

    ShowHostCastCapabilityReport capability{};
    capability.struct_size = 0;
    expect(!show_host_get_cast_capability_report(host, &capability),
        "cast capability report accepted struct_size 0");
    capability.struct_size = sizeof(capability);
    capability.reserved = 1;
    expect(!show_host_get_cast_capability_report(host, &capability),
        "cast capability report accepted a non-zero reserved field");
    capability.reserved = 0;
    expect(show_host_get_cast_capability_report(host, &capability),
        "cast capability report rejected a well-formed struct");
    expect(!capability.backend_active,
        "cast capability report claims an active backend while idle");

    ShowHostStageOverlayState overlay{};
    overlay.struct_size = 0;
    expect(!show_host_set_stage_overlay_state(host, &overlay),
        "stage overlay accepted struct_size 0");
    overlay.struct_size = sizeof(overlay);
    overlay.flags = 0xFFFFFFFFU;
    expect(!show_host_set_stage_overlay_state(host, &overlay),
        "stage overlay accepted unknown flags");

    milestone("loading missing media must not invent a running program");
    const auto state_before_failed_load = show_host_get_state(host);
    const bool loaded = show_host_load(host, kMissingMedia, kMissingMedia,
        kMissingMedia, kMissingMedia);
    note("load(missing media) -> " + std::string(loaded ? "true" : "false")
        + ", state " + std::to_string(state_before_failed_load) + " -> "
        + std::to_string(show_host_get_state(host))
        + ", decoder owners "
        + std::to_string(show_host_get_active_video_decoder_owners(host)));
    expect(!loaded, "loading missing media reported success");
    expect(show_host_get_state(host) == SHOW_STATE_IDLE,
        "a failed load did not leave the host IDLE");
    expect(show_host_get_active_video_decoder_owners(host) == 0,
        "a failed load left a video decoder owner behind");
    expect(!show_host_is_buffering(host),
        "a failed load left the host buffering");
    expect(show_host_get_cast_stream_port(host) == 0,
        "a failed load opened a Cast port");

    milestone("destroy on an idle host completes");
    show_host_destroy(host);

    std::cout << "[host-lifecycle] all contract checks passed" << std::endl;
    return EXIT_SUCCESS;
}
