// Mechanically split out of host_windows.h:
// the definitions below are unchanged, only their translation unit moved.

#include "host_windows.h"

#include <utility>

// namespace

namespace {

class StageThreadDpiAwareness final {
public:
    StageThreadDpiAwareness() noexcept
        : previous_(set_thread_dpi_awareness_context_if_available(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}

    ~StageThreadDpiAwareness() {
        if (previous_) {
            static_cast<void>(
                set_thread_dpi_awareness_context_if_available(previous_));
        }
    }

    StageThreadDpiAwareness(const StageThreadDpiAwareness&) = delete;
    StageThreadDpiAwareness& operator=(
        const StageThreadDpiAwareness&) = delete;

private:
    DPI_AWARENESS_CONTEXT previous_{};
};

template <typename Function>
decltype(auto) with_cast_worker_fenced(ShowHost& host, Function&& function) {
    // Short interactive clock/audio commands remain ordered with the frame
    // that observes them. Program load deliberately does not use this fence:
    // it publishes an immutable CastProgramSnapshot so a decoder synchronize
    // in the previous frame can never block the synchronous load ABI.
    std::lock_guard lock(host.cast.worker_mutex);
    return std::forward<Function>(function)();
}

}  // namespace

LRESULT CALLBACK video_blank_proc(
    HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* host = reinterpret_cast<ShowHost*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        host = reinterpret_cast<ShowHost*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(host));
        return TRUE;
    }
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_PAINT:
        if (host && host->native_stage.unified_stage_presenter.initialized()) {
            host->native_stage.force_stage_redraw = true;
            PAINTSTRUCT paint{};
            BeginPaint(hwnd, &paint);
            EndPaint(hwnd, &paint);
            return 0;
        }
        break;
    case WM_ERASEBKGND: {
        if (host && host->native_stage.unified_stage_presenter.initialized()) return TRUE;
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect(reinterpret_cast<HDC>(wparam), &rect,
            reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return TRUE;
    }
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}


LRESULT CALLBACK media_clock_proc(
    HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return TRUE;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}



LRESULT CALLBACK stage_proc(
    HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* host = reinterpret_cast<ShowHost*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        host = reinterpret_cast<ShowHost*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
        host->window = hwnd;
        return TRUE;
    }
    case WM_CREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        host = reinterpret_cast<ShowHost*>(create->lpCreateParams);
        auto* instance = reinterpret_cast<HINSTANCE>(
            GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
        const auto video_blank = CreateWindowExW(
            WS_EX_NOACTIVATE,
            kVideoBlankClass, nullptr,
            WS_CHILD,
            0, 0, 1, 1, hwnd, nullptr, instance, host);
        if (video_blank) {
            host->stage_surface_window = video_blank;
            resize_stage_surface(*host);
        }
        return 0;
    }
    case WM_NCHITTEST:
        // Stage is video-only; never capture mouse.  Without this
        // Windows shows the "not responding" cursor when the mouse
        // hovers over the stage window.
        return HTTRANSPARENT;
    case WM_SIZE:
        if (host) {
            host->native_stage.force_stage_redraw = true;
            RECT rect{};
            GetClientRect(hwnd, &rect);
            resize_media_player_for_stage(*host, rect);
            if (IsWindowVisible(host->stage_surface_window)) {
                resize_stage_surface(*host);
            }
        }
        return 0;
    case WM_DISPLAYCHANGE:
        if (host && !has_ts_stage_output(*host)) {
            host->native_stage.force_stage_redraw = true;
            // Refresh-rate changes do not invalidate a D3D11 device and must
            // not tear down the decoder or Unified Stage. DXGI follows the
            // HWND's new scanout mode; a later Stage rect update handles real
            // resolution/topology changes through normal profile matching.
            const bool device_removed = host->native_stage.preferred_stage_device
                && FAILED(host->native_stage.preferred_stage_device
                    ->GetDeviceRemovedReason());
            if (device_removed) {
                reset_unified_stage_device_pipeline(*host, true);
            }
            apply_stage_layout(*host);
        }
        return 0;
    case kHostLoadMessage:
        return host && lparam
            ? (handle_load(*host, *reinterpret_cast<LoadRequest*>(lparam))
                ? TRUE : FALSE)
            : FALSE;
    case kHostPrepareNextMessage:
        return host && lparam
            ? (handle_prepare_next(
                *host, *reinterpret_cast<LoadRequest*>(lparam))
                ? TRUE : FALSE)
            : FALSE;
    case kHostStageDeviceMessage:
        return host && lparam
            ? (handle_stage_device(
                *host, reinterpret_cast<void*>(lparam)) ? TRUE : FALSE)
            : FALSE;
    case kHostPlayMessage:
        if (host) with_cast_worker_fenced(
            *host, [&] { handle_play(*host); });
        return 0;
    case kHostPauseMessage:
        if (host) with_cast_worker_fenced(
            *host, [&] { handle_pause(*host); });
        return 0;
    case kHostStopMessage:
        if (host) with_cast_worker_fenced(
            *host, [&] { handle_stop(*host); });
        return 0;
    case kHostSeekMessage:
        if (host && lparam) with_cast_worker_fenced(*host, [&] {
            handle_seek(*host, *reinterpret_cast<double*>(lparam));
        });
        return 0;
    case kHostVolumeMessage:
        if (host) with_cast_worker_fenced(*host, [&] {
            handle_volume(
                *host, static_cast<int>(static_cast<LONG_PTR>(lparam)));
        });
        return 0;
    case kHostKeyMessage:
        if (host) with_cast_worker_fenced(*host, [&] {
            handle_key(
                *host, static_cast<int>(static_cast<LONG_PTR>(lparam)));
        });
        return 0;
    case kHostAudioClockOffsetMessage:
        if (host && lparam) {
            with_cast_worker_fenced(*host, [&] {
                handle_audio_clock_offset(
                    *host, *reinterpret_cast<double*>(lparam));
            });
        }
        return 0;
    case kHostAudioTrackMessage:
        return host ? with_cast_worker_fenced(*host, [&] {
            return handle_audio_track(*host, static_cast<int>(wparam))
                ? TRUE : FALSE;
        }) : FALSE;
    case kHostStageVisibleMessage:
        if (host) set_stage_visible(*host, wparam != FALSE);
        return 0;
    case kHostStageOverlayStateMessage:
        return host && lparam
            ? (handle_stage_overlay_state(
                *host, *reinterpret_cast<StageOverlayRequest*>(lparam))
                ? TRUE : FALSE)
            : FALSE;
    case kHostStageWindowRectMessage:
        if (host && lparam) {
            apply_stage_window_rect(
                *host, *reinterpret_cast<StageWindowRectRequest*>(lparam));
        }
        return 0;
    case kHostStartCastStreamMessage:
        return host && handle_start_cast_stream(
            *host, static_cast<std::uint16_t>(wparam)) ? TRUE : FALSE;
    case kHostStopCastStreamMessage:
        if (host) handle_stop_cast_stream(*host);
        return 0;
    case kCastMediaEndedMessage:
        if (host) with_cast_worker_fenced(*host, [&] {
            const auto revision = host->playback.video_timeline_revision.load(
                std::memory_order_relaxed);
            if (revision == static_cast<std::uint64_t>(lparam)
                    && host->playback.state.load() == SHOW_STATE_PLAYING) {
                handle_media_ended(*host);
            }
        });
        return 0;
    case kCastFatalErrorMessage:
        if (host) with_cast_worker_fenced(*host, [&] {
            const auto revision = host->playback.video_timeline_revision.load(
                std::memory_order_relaxed);
            if (revision != static_cast<std::uint64_t>(lparam)) return;
            // Cast opens progressive sources asynchronously, so load() cannot
            // report a decoder failure that happens after publication. Keep
            // the existing ABI and publish the terminal state on the host.
            host->playback.state.store(SHOW_STATE_STOPPED);
            host->playback.video_buffering.store(false);
            host->output_transition.cast_video_source_active.store(
                false, std::memory_order_release);
            fail_program_transition(*host);
            host->playback.paused_position = 0.0;
            host->playback.has_paused_position = false;
            bump_playback_revision(*host);
        });
        return 0;
    case kVideoEventMessage:
        if (host) {
            // Cast closes MediaEngine and owns the only active video decoder.
            // Ignore any notification that was already queued while the
            // output lease changed; in particular, never rebuild a hidden
            // MediaEngine from a stale error event.
            if (has_ts_stage_output(*host)) return 0;
            if (static_cast<std::uint64_t>(lparam) != host->playback.player.generation) {
                return 0;
            }
            if (wparam == MF_MEDIA_ENGINE_EVENT_ERROR
                    || wparam == MF_MEDIA_ENGINE_EVENT_STREAMRENDERINGERROR) {
                // A transient MediaEngine failure must not discard a complete
                // native program that has already reached the physical sink.
                // The independent Stage decoder can continue or recover while
                // the clock player is reopened below.
                if (!has_current_native_video_program(*host)) {
                    fail_program_transition(*host);
                }
                handle_video_waiting(*host);
                log_line("MF media engine error; rebuilding video pipeline");
                const auto position = media_time(*host);
                const bool should_play =
                    host->playback.state.load() == SHOW_STATE_PLAYING;
                const auto video_output_window = host->media_clock_window
                    ? host->media_clock_window : host->window;
                if (host->playback.player.reopen_current(
                        host->window,
                        video_output_window,
                        position,
                        should_play)) {
                    host->output_transition.media_engine_video_source_active.store(
                        !host->playback.player.source_path.empty(),
                        std::memory_order_release);
                    RECT rect{};
                    GetClientRect(host->playback.player.playback_window, &rect);
                    host->playback.player.resize(rect);
                } else {
                    host->output_transition.media_engine_video_source_active.store(
                        false, std::memory_order_release);
                    log_line("MF media engine rebuild failed");
                }
                return 0;
            }
            if (wparam == MF_MEDIA_ENGINE_EVENT_FORMATCHANGE) {
                // Video format resolved; ensure the render area is correct.
                RECT rect{};
                GetClientRect(host->playback.player.playback_window, &rect);
                host->playback.player.resize(rect);
            }
            if (wparam == MF_MEDIA_ENGINE_EVENT_WAITING
                    || wparam == MF_MEDIA_ENGINE_EVENT_STALLED) {
                handle_video_waiting(*host);
            }
            if (wparam == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA
                    || wparam == MF_MEDIA_ENGINE_EVENT_CANPLAY
                    || wparam == MF_MEDIA_ENGINE_EVENT_CANPLAYTHROUGH
                    || wparam == MF_MEDIA_ENGINE_EVENT_LOADEDDATA
                    || wparam == MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY) {
                host->playback.player.apply_pending_seek();
                host->playback.player.try_play_if_requested();
                handle_video_can_play(*host);
            }
            if (wparam == MF_MEDIA_ENGINE_EVENT_PLAYING) {
                handle_video_can_play(*host);
            }
            if (wparam == MF_MEDIA_ENGINE_EVENT_ENDED) {
                host->playback.player.mark_ended();
                if (!host->playback.audio->is_open() || host->playback.video_master_clock) {
                    handle_media_ended(*host);
                }
            }
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (host) {
            host->render_loop_active = false;
            release_unified_native_stage_frames(*host);
            host->native_stage.unified_stage_presenter.shutdown();
            if (host->media_clock_window) {
                DestroyWindow(host->media_clock_window);
                host->media_clock_window = nullptr;
            }
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}



void stage_thread_main(ShowHost* host) {
    host->thread.id = GetCurrentThreadId();
    // Scope high-DPI awareness to Show's dedicated Stage thread. The Flutter
    // controller intentionally stays DPI-unaware, while the top-level Stage
    // HWND and its swap chain must use the target monitor's physical pixels.
    StageThreadDpiAwareness dpi_awareness;
    StageThreadSchedulingScope scheduling_scope;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto mf = MFStartup(MF_VERSION);
    const auto instance = GetModuleHandleW(nullptr);

    WNDCLASSW stage_class{};
    stage_class.hInstance = instance;
    stage_class.lpfnWndProc = stage_proc;
    stage_class.lpszClassName = kStageClass;
    stage_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    stage_class.hbrBackground = reinterpret_cast<HBRUSH>(
        GetStockObject(BLACK_BRUSH));
    RegisterClassW(&stage_class);

    WNDCLASSW video_blank_class{};
    video_blank_class.hInstance = instance;
    video_blank_class.lpfnWndProc = video_blank_proc;
    video_blank_class.lpszClassName = kVideoBlankClass;
    video_blank_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    video_blank_class.hbrBackground = reinterpret_cast<HBRUSH>(
        GetStockObject(BLACK_BRUSH));
    RegisterClassW(&video_blank_class);

    WNDCLASSW media_clock_class{};
    media_clock_class.hInstance = instance;
    media_clock_class.lpfnWndProc = media_clock_proc;
    media_clock_class.lpszClassName = kMediaClockClass;
    media_clock_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    media_clock_class.hbrBackground = reinterpret_cast<HBRUSH>(
        GetStockObject(BLACK_BRUSH));
    RegisterClassW(&media_clock_class);

    host->thread.startup_failed = FAILED(mf);
    if (!host->thread.startup_failed) {
        constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        RECT window_rect{
            0, 0, static_cast<LONG>(kInitialWidth),
            static_cast<LONG>(kInitialHeight),
        };
        AdjustWindowRectEx(&window_rect, style, FALSE, 0);
        const auto window = CreateWindowExW(0, kStageClass,
            L"Kirakara Stage Output", style,
            CW_USEDEFAULT, CW_USEDEFAULT,
            window_rect.right - window_rect.left,
            window_rect.bottom - window_rect.top,
            nullptr, nullptr, instance, host);
        host->thread.startup_failed = !window;
        if (window) {
            ShowWindow(window, SW_HIDE);
        }
        const auto media_clock_window = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kMediaClockClass,
            nullptr,
            WS_POPUP,
            -32000,
            -32000,
            16,
            16,
            nullptr,
            nullptr,
            instance,
            host);
        if (!media_clock_window) {
            host->thread.startup_failed = true;
        } else {
            host->media_clock_window = media_clock_window;
            ShowWindow(media_clock_window, SW_SHOWNOACTIVATE);
        }
    }

    {
        std::lock_guard lock(host->thread.ready_mutex);
        host->thread.ready = true;
    }
    host->thread.ready_cv.notify_all();

    MSG message{};
    bool running = !host->thread.startup_failed;
    StageRenderWaiter render_waiter;
    auto next_render_due = std::chrono::steady_clock::now();
    while (running) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running) break;
        // Cast owns a dedicated waitable-timer loop. The host thread must not
        // keep waking at the Native 60 Hz cadence while the TS lease is held.
        if (host->render_loop_active && host->window
                && !has_ts_stage_output(*host)) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_render_due) {
                render_idle_frame(*host);
                next_render_due += kStageRenderInterval;
                const auto completed = std::chrono::steady_clock::now();
                std::uint64_t missed_slots{};
                if (next_render_due <= completed) {
                    const auto missed =
                        (completed - next_render_due) / kStageRenderInterval
                        + 1;
                    missed_slots = static_cast<std::uint64_t>(missed);
                    host->native_stage.pipeline_diagnostics.increment(
                        NativeStagePipelineCounter::missed_render_slots,
                        missed_slots);
                    next_render_due += kStageRenderInterval * missed;
                }
                const auto previous_preview_level =
                    host->native_stage.preview_load_policy.level();
                host->native_stage.preview_load_policy.observe(
                    has_physical_stage_output(*host),
                    completed,
                    missed_slots);
                const auto current_preview_level =
                    host->native_stage.preview_load_policy.level();
                if (current_preview_level > previous_preview_level) {
                    host->native_stage.pipeline_diagnostics.increment(
                        current_preview_level
                                == NativeStagePreviewLoadLevel::reduced_20_fps
                            ? NativeStagePipelineCounter::
                                preview_degraded_to_20
                            : NativeStagePipelineCounter::
                                preview_degraded_to_15);
                } else if (current_preview_level
                        < previous_preview_level) {
                    host->native_stage.pipeline_diagnostics.increment(
                        NativeStagePipelineCounter::preview_load_recoveries);
                }
                host->native_stage.pipeline_diagnostics.maybe_report();
            }
            render_waiter.wait_until(next_render_due);
        } else {
            next_render_due = std::chrono::steady_clock::now();
            WaitMessage();
        }
    }

    handle_stop_cast_stream(*host);
    host->native_stage.stage_video_decoder.stop();
    host->native_stage.stage_video_decoder_started = false;
    host->native_stage.standby_video_decoder.stop();
    host->native_stage.standby_video_decoder_started = false;
    release_unified_native_stage_frames(*host);
    host->native_stage.unified_stage_presenter.shutdown();
    host->native_stage.unified_stage_renderer.retire();
    host->native_stage.unified_stage_renderer =
        kirakara::show::win32::UnifiedStageRenderer{};
    if (host->media_clock_window) {
        DestroyWindow(host->media_clock_window);
        host->media_clock_window = nullptr;
    }
    static_cast<void>(
        host->output_transition.stage_output_lease.release(StageOutputMode::physical_display));
    host->playback.audio->close();
    host->playback.player.close();
    if (SUCCEEDED(mf)) MFShutdown();
    CoUninitialize();
}



ShowHost* as_host(ShowHostHandle handle) {
    return static_cast<ShowHost*>(handle);
}
