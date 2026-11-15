#include "host_hud.h"
#include "../host/host_data.h"

#include <algorithm>
#include <cmath>

namespace {
void cancel_animation_timer(ShowHost& host) {
    if (host.hud.animation_timer_active) {
        KillTimer(host.window, kHostHudAnimationTimer);
        host.hud.animation_timer_active = false;
        host.hud.animation_timer_deadline = 0;
    }
}

void schedule_animation_timer(ShowHost& host, double now, double deadline) {
    if (host.render_loop_active || deadline <= 0) {
        cancel_animation_timer(host);
        return;
    }
    if (host.hud.animation_timer_active && host.hud.animation_timer_deadline <= deadline) return;
    const auto delay = static_cast<UINT>(std::clamp(
        std::ceil((deadline - now) * 1000), 1.0, 3200.0));
    host.hud.animation_timer_active =
        SetTimer(host.window, kHostHudAnimationTimer, delay, nullptr) != 0;
    host.hud.animation_timer_deadline = deadline;
}
}

void request_host_hud_refresh(ShowHost& host) {
    if (!host.hud.refresh_pending.exchange(true)
            && !PostMessageW(host.window, kHostHudStateMessage, 0, 0)) {
        host.hud.refresh_pending.store(false);
    }
}

void refresh_host_lan_state(ShowHost& host) {
    const auto available = host.hud.network.refresh();
    if (!available) return; // Keep the last known state after an OS query error.
    host.hud.lan_available.store(*available, std::memory_order_release);
    const auto revision = host.hud.network.revision();
    if (host.hud.lan_revision.exchange(revision, std::memory_order_acq_rel) == revision) return;
    if (host.hud.callback) host.hud.callback(host.hud.callback_context, revision, *available);
    render_physical_hud(host);
}
void render_physical_hud(ShowHost& host) {
    if (has_ts_stage_output(host)) {
        cancel_animation_timer(host);
        static_cast<void>(host.hud.physical_output.present({}));
        return;
    }
    const auto playback_state = host.playback.state.load();
    const bool active = host.playback.current_media.loaded && (playback_state == SHOW_STATE_PLAYING
        || playback_state == SHOW_STATE_PAUSED || host.output_transition.native_transition_holding);
    const bool advancing = playback_state == SHOW_STATE_PLAYING && !host.playback.video_buffering.load()
        && !host.native_stage.native_video_start_pending;
    const auto now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto presentation = host.hud.controller.update({
        host.hud.program_instance.load(), host.hud.discontinuity_revision.load(),
        host.playback.position.load(), host.playback.duration.load(), now, active, advancing,
        host.native_stage.stage_program_ready || (active && host.playback.current_media.video.empty() && advancing),
        host.output_transition.native_transition_holding});
    if (!has_physical_stage_output(host)) {
        cancel_animation_timer(host);
        static_cast<void>(host.hud.physical_output.present({}));
        return;
    }
    auto* device = host.native_stage.unified_stage_renderer.native_d3d_device();
    const auto profile = host.native_stage.unified_stage_renderer.profile();
    const auto state = host.hud.state.load();
    const auto qr = host.hud.qr.load();
    HostHudLayers layers;
    layers.card = host.hud.physical_renderer.render(device,
        profile.width, profile.height, state.get(), qr,
        host.hud.lan_available.load() && qr && qr->network_revision == host.hud.lan_revision.load(), presentation);
    const auto event = host.hud.action.load();
    const auto appearance = evaluate_host_hud_action(event.get(), now);
    layers.action = host.hud.action_renderer.render(
        device, profile.width, profile.height, layers.card, appearance);
    // While idle, wait until the fade starts. During a fade, tick only the HUD.
    double deadline = layers.action.visible ? appearance.next_deadline : 0;
    if (layers.card.animating || layers.action.animating) {
        const auto next_tick = now + .016;
        deadline = deadline > 0 ? std::min(deadline, next_tick) : next_tick;
    }
    schedule_animation_timer(host, now, deadline);
    if (!layers.card.visible && !layers.action.visible) {
        static_cast<void>(host.hud.physical_output.present({}));
    } else if (host.hud.physical_output.initialize(host.stage_surface_window, device)) {
        static_cast<void>(host.hud.physical_output.present(layers));
    }
}

void service_host_hud_timer(ShowHost& host) {
    cancel_animation_timer(host);
    render_physical_hud(host);
}

void release_physical_hud(ShowHost& host) {
    cancel_animation_timer(host);
    host.hud.physical_output.shutdown();
    host.hud.physical_renderer.reset();
    host.hud.action_renderer.reset();
}
