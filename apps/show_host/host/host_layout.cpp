// Mechanically split out of host_layout.h:
// the definitions below are unchanged, only their translation unit moved.

#include "host_layout.h"


namespace {

class ScopedThreadDpiAwareness final {
public:
    explicit ScopedThreadDpiAwareness(DPI_AWARENESS_CONTEXT awareness) noexcept
        : previous_(set_thread_dpi_awareness_context_if_available(awareness)) {}

    ~ScopedThreadDpiAwareness() {
        if (previous_) {
            static_cast<void>(
                set_thread_dpi_awareness_context_if_available(previous_));
        }
    }

    ScopedThreadDpiAwareness(const ScopedThreadDpiAwareness&) = delete;
    ScopedThreadDpiAwareness& operator=(
        const ScopedThreadDpiAwareness&) = delete;

    [[nodiscard]] bool active() const noexcept { return previous_ != nullptr; }

private:
    DPI_AWARENESS_CONTEXT previous_{};
};

[[nodiscard]] bool same_rect(const RECT& left, const RECT& right) noexcept {
    return left.left == right.left && left.top == right.top
        && left.right == right.right && left.bottom == right.bottom;
}

// The public Stage API is expressed in physical desktop pixels. The Flutter
// controller intentionally remains DPI-unaware, though, so EnumDisplayMonitors
// returns a virtualized full-monitor rectangle on mixed-DPI desktops. Accept
// that one legacy shape without treating arbitrary caller rectangles as DIPs:
// identify the exact monitor under an unaware context, then query the same
// HMONITOR again under the Stage thread's Per-Monitor-V2 context.
[[nodiscard]] RECT normalize_full_monitor_rect_to_physical(
        const RECT& requested) noexcept {
    HMONITOR monitor{};
    MONITORINFO virtual_info{};
    virtual_info.cbSize = sizeof(virtual_info);
    {
        ScopedThreadDpiAwareness unaware(DPI_AWARENESS_CONTEXT_UNAWARE);
        if (!unaware.active()) return requested;
        monitor = MonitorFromRect(&requested, MONITOR_DEFAULTTONULL);
        if (!monitor || !GetMonitorInfoW(monitor, &virtual_info)
                || !same_rect(requested, virtual_info.rcMonitor)) {
            return requested;
        }
    }

    MONITORINFO physical_info{};
    physical_info.cbSize = sizeof(physical_info);
    return GetMonitorInfoW(monitor, &physical_info)
        ? physical_info.rcMonitor : requested;
}

}  // namespace


std::vector<std::shared_ptr<StageTextureSourceState>>
live_stage_texture_sources(ShowHost& host) {
    std::vector<std::shared_ptr<StageTextureSourceState>> result;
    std::lock_guard lock(host.native_stage.stage_texture_sources_mutex);
    auto output = host.native_stage.stage_texture_sources.begin();
    for (auto input = host.native_stage.stage_texture_sources.begin();
            input != host.native_stage.stage_texture_sources.end(); ++input) {
        if (auto source = input->lock()) {
            result.push_back(source);
            *output++ = *input;
        }
    }
    host.native_stage.stage_texture_sources.erase(
        output, host.native_stage.stage_texture_sources.end());
    return result;
}


bool has_active_stage_texture_source(ShowHost& host) {
    for (const auto& source : live_stage_texture_sources(host)) {
        std::lock_guard lock(source->mutex);
        if (source->alive && source->active
                && (source->transport
                        == StagePreviewTransport::flutter_external_texture
                    || source->visual_callback != nullptr)) {
            return true;
        }
    }
    return false;
}

bool has_active_external_texture_source(ShowHost& host) {
    for (const auto& source : live_stage_texture_sources(host)) {
        std::lock_guard lock(source->mutex);
        if (source->alive && source->active
                && source->transport
                    == StagePreviewTransport::flutter_external_texture) {
            return true;
        }
    }
    return false;
}



void resize_media_player_for_stage(ShowHost& host, const RECT& stage_rect) {
    RECT media_rect{};
    if (host.media_clock_window
            && GetClientRect(host.media_clock_window, &media_rect)) {
        host.playback.player.resize(media_rect);
        return;
    }
    host.playback.player.resize(stage_rect);
}



void attach_stage_window(ShowHost& host) {
    if (!host.window) return;
    const auto rect = active_stage_rect(host);
    const auto width = std::max<LONG>(1, rect.right - rect.left);
    const auto height = std::max<LONG>(1, rect.bottom - rect.top);

    SetParent(host.window, nullptr);
    SetWindowLongPtrW(host.window, GWL_STYLE,
        WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
    SetWindowLongPtrW(host.window, GWL_EXSTYLE,
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    SetWindowPos(host.window, HWND_NOTOPMOST,
        rect.left, rect.top, width, height,
        SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    ShowWindow(host.window, SW_SHOWNOACTIVATE);

    RECT client{0, 0, width, height};
    resize_media_player_for_stage(host, client);
}



void apply_stage_layout(ShowHost& host) {
    if (has_ts_stage_output(host)) {
        if (host.window) ShowWindow(host.window, SW_HIDE);
        return;
    }

    if (!has_physical_stage_output(host)) {
        if (host.window) ShowWindow(host.window, SW_HIDE);
        return;
    }

    bool need_attach = true;
    if (host.window) {
        RECT current{};
        GetWindowRect(host.window, &current);
        const auto target = active_stage_rect(host);
        const auto style = static_cast<DWORD>(
            GetWindowLongPtrW(host.window, GWL_STYLE));
        const auto ex_style = static_cast<DWORD>(
            GetWindowLongPtrW(host.window, GWL_EXSTYLE));
        if (GetParent(host.window) == nullptr
                && IsWindowVisible(host.window)
                && (style & WS_POPUP) != 0
                && (style & WS_CHILD) == 0
                && (ex_style & WS_EX_NOACTIVATE) != 0
                && current.left == target.left
                && current.top == target.top
                && (current.right - current.left)
                    == (target.right - target.left)
                && (current.bottom - current.top)
                    == (target.bottom - target.top)) {
            need_attach = false;
        }
    }
    if (need_attach) attach_stage_window(host);
}



bool set_stage_visible(ShowHost& host, bool visible) {
    const bool was_physical = has_physical_stage_output(host);
    if (visible) {
        if (!host.output_transition.stage_output_lease.try_acquire(
                StageOutputMode::physical_display)) {
            return false;
        }
    } else {
        static_cast<void>(host.output_transition.stage_output_lease.release(
            StageOutputMode::physical_display));
    }
    const bool is_physical = has_physical_stage_output(host);
    if (was_physical != is_physical) {
        host.native_stage.preview_load_policy.reset();
    }
    if (!host.window) return true;
    apply_stage_layout(host);
    if (visible) {
        // A physical output lease may be acquired before the first program is
        // loaded. Paint the already-created top-level and surface HWNDs now;
        // their class/background handlers produce deterministic black without
        // starting the 60 Hz render loop. The swap-chain presenter takes over
        // through the unchanged first-frame path after a successful load.
        InvalidateRect(host.window, nullptr, TRUE);
        UpdateWindow(host.window);
        if (host.stage_surface_window) {
            InvalidateRect(host.stage_surface_window, nullptr, TRUE);
            UpdateWindow(host.stage_surface_window);
        }
    }
    return true;
}



void apply_stage_window_rect(
        ShowHost& host, const StageWindowRectRequest& request) {
    if (!host.window || request.width == 0 || request.height == 0) return;
    const RECT requested{
        request.x,
        request.y,
        request.x + static_cast<LONG>(request.width),
        request.y + static_cast<LONG>(request.height),
    };
    host.stage_rect = normalize_full_monitor_rect_to_physical(requested);
    host.stage_rect_valid = true;
    apply_stage_layout(host);
}



void resize_stage_surface(ShowHost& host) {
    if (!host.window || !host.stage_surface_window) return;
    RECT rect{};
    GetClientRect(host.window, &rect);
    SetWindowPos(host.stage_surface_window, HWND_TOP,
        0, 0, rect.right - rect.left, rect.bottom - rect.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (host.native_stage.unified_stage_presenter.initialized()) {
        static_cast<void>(host.native_stage.unified_stage_presenter.resize(
            static_cast<std::uint32_t>(std::max<LONG>(
                1, rect.right - rect.left)),
            static_cast<std::uint32_t>(std::max<LONG>(
                1, rect.bottom - rect.top))));
    }
}



void ensure_stage_surface_visible(ShowHost& host) {
    if (!host.stage_surface_window) return;
    resize_stage_surface(host);
    ShowWindow(host.stage_surface_window, SW_SHOWNOACTIVATE);
}
