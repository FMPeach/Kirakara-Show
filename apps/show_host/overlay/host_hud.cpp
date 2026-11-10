#include "host_hud.h"
#include "../host/host_data.h"

void refresh_host_lan_state(ShowHost& host) {
    const bool available = host.hud.network.refresh();
    host.hud.lan_available.store(available, std::memory_order_release);
    const auto revision = host.hud.lan_revision.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (host.hud.callback) host.hud.callback(host.hud.callback_context, revision, available);
    render_physical_hud(host);
}
void render_physical_hud(ShowHost& host) {
    if (!has_physical_stage_output(host) || has_ts_stage_output(host)) {
        static_cast<void>(host.hud.physical_output.set_visible(false));
        return;
    }
    auto* device = host.native_stage.unified_stage_renderer.native_d3d_device();
    const auto profile = host.native_stage.unified_stage_renderer.profile();
    const auto state = host.hud.state.load();
    const auto qr = host.hud.qr.load();
    const auto& frame = host.hud.physical_renderer.render(device,
        profile.width, profile.height, state.get(), qr, host.hud.lan_available.load());
    if (!frame.visible) {
        static_cast<void>(host.hud.physical_output.set_visible(false));
        return;
    }
    if (host.hud.physical_output.initialize(host.stage_surface_window, device)) {
        static_cast<void>(host.hud.physical_output.present(frame.texture, static_cast<float>(frame.x),
            static_cast<float>(frame.y), frame.revision));
    }
}
void release_physical_hud(ShowHost& host) {
    host.hud.physical_output.shutdown();
    host.hud.physical_renderer.reset();
}
