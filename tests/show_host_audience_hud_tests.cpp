#include "../apps/show_host/host/host_data.h"
#include "hud_test_image.h"
#include "../apps/show_host/overlay/host_hud_blitter.h"
#include <cstdlib>
#include <iostream>

namespace {
void expect(bool value, const char* message) {
    if (value) return;
    std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE);
}
}
int main() {
    expect(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM");
    {
        auto image = make_hud_test_image();
        expect(image && image->valid(), "bounded PNG fixture");
        kirakara::show::win32::D3D11TextureSurface device;
        expect(device.resize(16, 16), "device");
        HostHudState state; state.options.qr_enabled = true; state.network_name = L"192.168.1.2:7391";
        HostHudRenderer renderer;
        auto frame = renderer.render(device.native_d3d_device(), 1920, 1080, &state, image, true);
        expect(frame.visible && frame.width == 500 && frame.height == 244
            && frame.x == 1370 && frame.y == 40, "1080p card geometry");
        for (int i = 0; i < 100; ++i) renderer.render(device.native_d3d_device(), 1920, 1080, &state, image, true);
        expect(renderer.rasterizations() == 1, "static card cached");
        expect(!renderer.render(device.native_d3d_device(), 1920, 1080, &state, image, false).visible,
            "no usable LAN hides entire card");
        expect(renderer.render(device.native_d3d_device(), 1920, 1080, &state, image, true).visible
            && renderer.rasterizations() == 1, "LAN recovery reuses cached card");
        state.options.qr_enabled = false;
        expect(!renderer.render(device.native_d3d_device(), 1920, 1080, &state, image, true).visible,
            "manual QR switch");
        state.options.qr_enabled = true;
        {
            kirakara::show::win32::D3D11TextureSurface target;
            expect(target.resize_on_device(device.native_d3d_device(), 640, 360), "Cast C target");
            target.begin_draw(); target.clear({.15F, .25F, .35F, 1});
            expect(target.end_draw(), "clean C frame");
            const auto original = target.read_rgba8();
            auto card = renderer.render(device.native_d3d_device(), 640, 360, &state, image, true);
            HostHudRetainedBgraLayer layer;
            bool changed{};
            expect(layer.apply(target.native_render_target(), card, true, changed) && changed,
                "Cast C draws cached card over clean base");
            expect(target.read_rgba8() != original, "card pixels visible in C");
            for (int i = 0; i < 100; ++i) expect(layer.apply(target.native_render_target(), card, false, changed)
                && !changed, "retained C frame skips all drawing");
            expect(layer.captures() == 1, "only one bounded underlay capture");
            card.visible = false;
            expect(layer.apply(target.native_render_target(), card, false, changed) && changed,
                "offline removes card from held frame");
            expect(target.read_rgba8() == original, "C restores exact underlying program pixels");
        }
        for (const auto& size : {std::pair{3840U, 2160U}, {1280U, 720U}, {960U, 540U}, {1024U, 576U}, {640U, 360U}}) {
            const auto& scaled = renderer.render(device.native_d3d_device(), size.first, size.second, &state, image, true);
            expect(scaled.visible && scaled.x + scaled.width <= size.first
                && scaled.y + scaled.height <= size.second, "bounded output scaling");
        }
        auto corrupt = std::make_shared<HostHudQrImage>(*image); corrupt->pixel_size = 165;
        expect(!renderer.render(device.native_d3d_device(), 1920, 1080, &state, corrupt, true).visible,
            "decoded dimensions must match metadata");
        const auto failed_draws = renderer.rasterizations();
        for (int i = 0; i < 100; ++i) expect(!renderer.render(device.native_d3d_device(), 1920, 1080, &state, corrupt, true).visible,
            "invalid image never exposes stale pixels");
        expect(renderer.rasterizations() == failed_draws, "invalid revision does not spin");

        expect(LanStateMonitor::usable_ipv4(0xc0a80102) && !LanStateMonitor::usable_ipv4(0x7f000001)
            && !LanStateMonitor::usable_ipv4(0xa9fe0001), "LAN is independent of Internet reachability");
        expect(LanStateMonitor::usable_adapter(L"Wi-Fi", 71, true)
            && !LanStateMonitor::usable_adapter(L"Radmin VPN", 6, true)
            && !LanStateMonitor::usable_adapter(L"Ethernet", 6, false), "adapter filtering");

        const auto handle = show_host_create(); expect(handle != nullptr, "Host");
        auto* host = static_cast<ShowHost*>(handle);
        ShowHostLanState lan{sizeof(lan), 1};
        expect(show_host_get_lan_state(handle, &lan) && lan.revision > 0, "initial LAN snapshot");
        lan.abi_version = 2; expect(!show_host_get_lan_state(handle, &lan), "version validation");
        wchar_t name[] = L"test network", title[] = L"next song";
        ShowHostHudQueueItem item{title, L"artist"};
        ShowHostAudienceHudState input{sizeof(input), 1, 3, 1, 1, name, &item};
        expect(show_host_set_audience_hud_state(handle, &input), "HUD input");
        name[0] = title[0] = L'X';
        expect(host->hud.state.load()->network_name == L"test network"
            && host->hud.state.load()->queue[0].title == L"next song", "borrowed strings copied");
        input.queue_count = 5; expect(!show_host_set_audience_hud_state(handle, &input), "queue cap");
        ShowHostPhoneOrderQrImage qr{sizeof(qr), 1, image->revision, image->pixel_size,
            image->modules, image->png.data(), static_cast<std::uint32_t>(image->png.size()), 0};
        expect(show_host_set_phone_order_qr_image(handle, &qr), "PNG accepted");
        image->png[8] ^= 1;
        expect(host->hud.qr.load()->png[8] != image->png[8], "PNG copied before return");
        qr.reserved = 1; expect(!show_host_set_phone_order_qr_image(handle, &qr), "reserved rejected");
        expect(show_host_set_phone_order_qr_image(handle, nullptr) && !host->hud.qr.load(), "clear image");
        expect(!host->hud.physical_output.initialized(), "controller-only mode creates no HUD visual");
        show_host_set_lan_state_callback(handle, nullptr, nullptr);
        show_host_destroy(handle);
    }
    CoUninitialize();
    std::cout << "Audience HUD cache, network gate, scaling and additive FFI passed.\n";
}
