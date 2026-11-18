#include "../apps/show_host/overlay/host_hud_renderer.h"
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
        HostHudLayers layers;
        layers.card = {card.native_texture(), 520, 20, 100, 60, 1, true};
        expect(output.present(layers), "present small HUD");
        for (int i = 0; i < 100; ++i) {
            expect(output.present(layers), "reuse static HUD");
        }
        expect(output.submissions() == 1, "static HUD never creates a frame pump");
        expect(output.present({}) && output.present(layers), "toggle card only");
        expect(output.submissions() == 1, "visibility keeps existing pixels");
        layers.card.x = 500; layers.card.y = 25; ++layers.card.revision;
        expect(output.present(layers), "refresh changed HUD");
        expect(output.submissions() == 2, "one present per content revision");
        for (int i = 0; i <= 10; ++i) {
            layers.card.opacity = i / 10.0F;
            expect(output.present(layers), "opacity-only composition update");
        }
        expect(output.submissions() == 2, "fades never copy or present the cached bitmap again");
        HostHudActionRenderer action;
        layers.card.layout_ready = true;
        layers.action = action.render(renderer.native_d3d_device(), 640, 360,
            layers.card, {HostHudAction::pause, 1});
        expect(layers.action.visible && output.present(layers)
            && output.submissions() == 3, "both tiles share the HWND target");
        const auto initial_y = layers.action.y;
        layers.card.y += 10;
        layers.action = action.render(renderer.native_d3d_device(), 640, 360,
            layers.card, {HostHudAction::pause, .5F});
        expect(layers.action.y == initial_y + 10 && output.present(layers)
            && output.submissions() == 3, "move and fade two cached tiles without another present");
        layers.card.visible = false;
        expect(output.present(layers) && output.submissions() == 3, "action survives independently");
        expect(output.present({}) && output.submissions() == 3, "hide both without new pixels");
        layers={};
        layers.announcement={card.native_texture(),15,10,100,60,1,true};
        expect(output.present(layers)&&output.submissions()==4,"independent announcement layer");
        for(int i=0;i<30;++i) expect(output.present(layers),"retain announcement pixels");
        expect(output.submissions()==4,"unchanged announcement does not copy");
        ++layers.announcement.revision;
        expect(output.present(layers)&&output.submissions()==5,"only announcement content is presented");
        expect(output.present({}),"remove announcement without changing base");
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
