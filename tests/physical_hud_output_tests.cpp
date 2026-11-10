#include "../apps/show_host/overlay/host_hud_state.h"
#include "../apps/show_host/overlay/physical_hud_output.h"
#include "kirakara/show/win32/d3d11_texture_surface.hpp"
#include "kirakara/show/win32/stage_window_presenter.hpp"
#include "kirakara/show/win32/unified_stage_renderer.hpp"

#include <windows.h>
#include <cstdlib>
#include <iostream>

using namespace kirakara::show;
using namespace kirakara::show::win32;

namespace {
void expect(bool value, const char* message) {
    if (value) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}
}

int main() {
    expect(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "initialize COM");
    const auto window = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"HUD test",
        WS_POPUP, -20000, -20000, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    expect(window != nullptr, "create existing Stage HWND");
    {
        UnifiedStageRenderer renderer;
        expect(renderer.configure({StageOutputRole::physical_display, 640, 360}),
            "configure unchanged program renderer");
        auto preview = renderer.subscribe();
        expect(renderer.render({.timing = {1, 0, 0, 166667},
                    .idle_color = {0.0F, 0.25F, 0.0F, 1.0F}})
                == UnifiedStageRenderResult::published, "publish clean program frame");
        auto clean = preview.take_latest();
        StageWindowPresenter presenter;
        expect(presenter.initialize(window, renderer.native_d3d_device())
                && presenter.present(clean, false), "present base on existing HWND");
        D3D11TextureSurface card;
        expect(card.resize_on_device(renderer.native_d3d_device(), 100, 60),
            "create small cached HUD texture");
        card.begin_draw();
        card.clear({0.5F, 0.0F, 0.0F, 0.5F});
        expect(card.end_draw(), "draw premultiplied test card");
        card.flush_d2d();
        PhysicalHudOutput output;
        expect(output.initialize(window, renderer.native_d3d_device()),
            "attach DComp above existing HWND swap chain");
        expect(output.present(card.native_texture(), 520, 20, 1), "present small HUD");
        for (int i = 0; i < 100; ++i) {
            expect(output.present(card.native_texture(), 520, 20, 1), "reuse static HUD");
        }
        expect(output.submissions() == 1, "static HUD never creates a frame pump");
        expect(output.set_visible(false) && output.set_visible(true), "toggle card only");
        expect(output.submissions() == 1, "visibility keeps existing pixels");
        expect(output.present(card.native_texture(), 500, 25, 2), "refresh changed HUD");
        expect(output.submissions() == 2, "one present per content revision");
        expect(renderer.rendered_frames() == 1 && !preview.peek_latest(),
            "HUD changes never render or publish clean preview frames");
        expect(clean->timing.generation == 1 && clean->timing.frame_id == 0,
            "HUD leaves program identity untouched");
        expect(presenter.repeat_last(false), "base presenter remains functional");
        output.shutdown();
        expect(presenter.repeat_last(false), "detaching HUD preserves base presenter");
        presenter.shutdown();
    }
    DestroyWindow(window);
    CoUninitialize();
    HostHudState state;
    expect(state.valid(), "empty HUD state is valid");
    state.options.queue_enabled = true;
    expect(!state.options.qr_enabled && state.options.queue_enabled,
        "QR and queue options are independent");
    state.queue.resize(kHostHudMaxQueueItems + 1);
    expect(!state.valid(), "queue payload is bounded");
    std::cout << "Host HUD: same-HWND DComp, static reuse and clean preview isolation passed.\n";
    return EXIT_SUCCESS;
}
