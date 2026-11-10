#include "../apps/show_host/host/host_data.h"
#include "hud_test_image.h"
#include "../apps/show_host/overlay/host_hud_blitter.h"
#include "../apps/show_host/overlay/host_hud_svg_icons.h"
#include <cstdlib>
#include <iostream>
#include <filesystem>

namespace {
void expect(bool value, const char* message) {
    if (value) return;
    std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE);
}
void save_card(const std::filesystem::path& path, const HostHudFrame& card, void* device) {
    using Microsoft::WRL::ComPtr;
    kirakara::show::win32::D3D11TextureSurface capture;
    expect(capture.resize_on_device(device, card.width, card.height), "snapshot surface");
    ComPtr<ID3D11DeviceContext> context;
    static_cast<ID3D11Device*>(device)->GetImmediateContext(&context);
    context->CopyResource(static_cast<ID3D11Texture2D*>(capture.native_texture()),
        static_cast<ID3D11Texture2D*>(card.texture));
    std::vector<BYTE> pixels(static_cast<std::size_t>(card.width) * card.height * 4);
    expect(capture.read_bgra8_into(pixels.data()), "snapshot readback");
    for (std::size_t i = 0; i < pixels.size(); i += 4) if (pixels[i + 3]) {
        for (int c = 0; c < 3; ++c) pixels[i + c] = static_cast<BYTE>(std::min(255U,
            static_cast<unsigned>(pixels[i + c]) * 255U / pixels[i + 3]));
    }
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    expect(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
        && SUCCEEDED(factory->CreateStream(&stream))
        && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))
        && SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))
        && SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))
        && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr))
        && SUCCEEDED(frame->SetSize(card.width, card.height)) && SUCCEEDED(frame->SetPixelFormat(&format))
        && SUCCEEDED(frame->WritePixels(card.height, card.width * 4, static_cast<UINT>(pixels.size()), pixels.data()))
        && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()), "write HUD snapshot");
}
}
int main(int argc, char** argv) {
    expect(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM");
    {
        auto image = make_hud_test_image();
        expect(image && image->valid(), "bounded PNG fixture");
        kirakara::show::win32::D3D11TextureSurface device;
        expect(device.resize(16, 16), "device");
        {
            HostHudSvgIcons icons;
            device.begin_draw();
            for (const auto icon : {HostHudIcon::wifi, HostHudIcon::phone, HostHudIcon::ethernet})
                expect(icons.draw(device.native_render_target(), icon, 0, 0, 12), "Lucide SVG supported");
            expect(device.end_draw(), "SVG draw batch");
        }
        HostHudState state; state.options.qr_enabled = true; state.network_name = L"示例 Wi-Fi"; state.network_wifi = true;
        HostHudRenderer renderer;
        auto frame = renderer.render(device.native_d3d_device(), 1920, 1080, &state, image, true);
        expect(frame.visible && frame.width == 500 && frame.height == 244
            && frame.x == 1370 && frame.y == 40, "1080p card geometry");
        if (argc > 1) {
            std::filesystem::create_directories(argv[1]);
            save_card(std::filesystem::path(argv[1]) / "qr-card.png", frame, device.native_d3d_device());
        }
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
        HostHudPresentation queue_phase{true, 1};
        state.current = {L"正在播放的歌曲", L"当前歌手"};
        state.queue = {{L"第一首超长歌名 第一首超长歌名 第一首超长歌名", L"歌手"},
            {L"第二首", L""}, {L"第三首", L"歌手"}, {L"第四首", L"歌手"}};
        for (const bool qr_enabled : {false, true}) for (const bool queue_enabled : {false, true}) {
            state.options = {qr_enabled, queue_enabled, false};
            expect(renderer.render(device.native_d3d_device(), 1920, 1080, &state, image, true, queue_phase).visible
                == (qr_enabled || queue_enabled), "independent switch combinations");
            expect(renderer.render(device.native_d3d_device(), 1920, 1080, &state, image, false, queue_phase).visible
                == queue_enabled, "offline only gates QR, not queue");
        }
        state.options = {false, true, false};
        const auto five_rows = renderer.render(device.native_d3d_device(), 1920, 1080, &state, {}, false, queue_phase);
        expect(five_rows.visible && five_rows.height == 455, "current plus four upcoming without QR or network");
        const auto text_draws = renderer.rasterizations();
        if (argc > 1) save_card(std::filesystem::path(argv[1]) / "queue-card.png",
            renderer.render(device.native_d3d_device(), 1920, 1080, &state, {}, false, queue_phase),
            device.native_d3d_device());
        for (int i = 100; i >= 0; --i) renderer.render(device.native_d3d_device(), 1920, 1080, &state, {}, false,
            {true, i / 100.0F});
        expect(renderer.rasterizations() == text_draws && renderer.progress_updates() > 1,
            "progress only paints a small stripe without font layout");
        state.current.title = L"新的当前歌曲";
        renderer.render(device.native_d3d_device(), 1920, 1080, &state, {}, false, queue_phase);
        expect(renderer.rasterizations() == text_draws + 1, "current metadata participates in card cache identity");
        for (const auto& size : {std::pair{3840U, 2160U}, {1280U, 720U}, {960U, 540U}, {1024U, 576U}, {640U, 360U}}) {
            const auto& scaled = renderer.render(device.native_d3d_device(), size.first, size.second, &state, {}, false, queue_phase);
            expect(scaled.visible && scaled.x + scaled.width <= size.first
                && scaled.y + scaled.height <= size.second, "five-row card fits scaled output");
        }
        state.queue.clear();
        expect(renderer.render(device.native_d3d_device(), 1920, 1080, &state, {}, false, queue_phase).visible,
            "empty queue has its own prompt");

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
        wchar_t name[] = L"test network", title[] = L"next song", current[] = L"current song";
        ShowHostHudQueueItem item{title, L"artist"};
        ShowHostAudienceHudState input{sizeof(input), 1, 3, 1, 1, name, &item, current, L"current artist"};
        expect(show_host_set_audience_hud_state(handle, &input), "HUD input");
        name[0] = title[0] = current[0] = L'X';
        expect(host->hud.state.load()->network_name == L"test network"
            && host->hud.state.load()->queue[0].title == L"next song"
            && host->hud.state.load()->current.title == L"current song", "borrowed strings copied");
        input.struct_size = offsetof(ShowHostAudienceHudState, current_title);
        expect(show_host_set_audience_hud_state(handle, &input)
            && host->hud.state.load()->current.title.empty(), "original HUD prefix remains accepted");
        ++input.struct_size;
        expect(!show_host_set_audience_hud_state(handle, &input), "partial trailing fields rejected");
        input.struct_size = sizeof(input);
        input.queue_count = 5; expect(!show_host_set_audience_hud_state(handle, &input), "queue cap");
        ShowHostPhoneOrderQrImage qr{sizeof(qr), 1, image->revision, image->pixel_size,
            image->modules, image->png.data(), static_cast<std::uint32_t>(image->png.size()), 0};
        expect(show_host_set_phone_order_qr_image(handle, &qr), "PNG accepted");
        image->png[8] ^= 1;
        expect(host->hud.qr.load()->png[8] != image->png[8], "PNG copied before return");
        qr.reserved = 1; expect(!show_host_set_phone_order_qr_image(handle, &qr), "reserved rejected");
        qr.reserved = 0;
        std::atomic<int> notifications{};
        show_host_set_lan_state_callback(handle, +[](void* context, std::uint64_t, bool) {
            static_cast<std::atomic<int>*>(context)->fetch_add(1);
        }, &notifications);
        SendMessageW(host->window, kHostLanChangedMessage, 0, 0);
        expect(notifications.load() > 0, "OS events are dispatched through the Host thread");
        expect(host->hud.qr.load()->network_revision != host->hud.lan_revision.load(),
            "old QR stays gated until App confirms current network image");
        expect(show_host_set_phone_order_qr_image(handle, &qr)
            && host->hud.qr.load()->network_revision == host->hud.lan_revision.load(),
            "unchanged image can confirm a new network epoch");
        show_host_set_lan_state_callback(handle, nullptr, nullptr);
        const auto count = notifications.load();
        SendMessageW(host->window, kHostLanChangedMessage, 0, 0);
        expect(notifications.load() == count, "callback unregister completes before return");
        expect(show_host_set_phone_order_qr_image(handle, nullptr) && !host->hud.qr.load(), "clear image");
        expect(!host->hud.physical_output.initialized(), "controller-only mode creates no HUD visual");
        show_host_set_lan_state_callback(handle, nullptr, nullptr);
        show_host_destroy(handle);
    }
    CoUninitialize();
    std::cout << "Audience HUD cache, network gate, scaling and additive FFI passed.\n";
}
