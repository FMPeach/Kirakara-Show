#include "../apps/show_host/host/host_data.h"
#include "hud_test_image.h"
#include "../apps/show_host/overlay/host_hud_blitter.h"
#include "../apps/show_host/overlay/host_hud_svg_icons.h"
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <filesystem>

namespace {
void expect(bool value, const char* message) {
    if (value) return;
    std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE);
}
std::vector<std::uint8_t> read_card(const HostHudFrame& card, void* device) {
    kirakara::show::win32::D3D11TextureSurface copy;
    expect(copy.resize_on_device(device, card.width, card.height), "card readback target");
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    static_cast<ID3D11Device*>(device)->GetImmediateContext(&context);
    context->CopyResource(static_cast<ID3D11Texture2D*>(copy.native_texture()),
        static_cast<ID3D11Texture2D*>(card.texture));
    return copy.read_rgba8();
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
        const auto qr_pixels = read_card(frame, device.native_d3d_device());
        const auto white = [&](int x, int y) {
            const auto i = (y * 500 + x) * 4;
            return qr_pixels[i] == 255 && qr_pixels[i + 1] == 255
                && qr_pixels[i + 2] == 255 && qr_pixels[i + 3] == 255;
        };
        expect(!white(33, 100) && !white(35, 35) && white(122, 34),
            "no extra white border and rounded PNG corners");
        expect(!white(122, 33) && white(122, 34) && white(122, 209) && !white(122, 210),
            "slightly enlarged white block preserves its center");
        const auto base = (225 * 500 + 300) * 4;
        const auto above_network_bottom = (199 * 500 + 360) * 4;
        const auto below_network_bottom = (211 * 500 + 360) * 4;
        expect(qr_pixels[above_network_bottom] != qr_pixels[base]
            && std::equal(qr_pixels.begin() + below_network_bottom,
                qr_pixels.begin() + below_network_bottom + 4, qr_pixels.begin() + base),
            "right-column text, icons and network card retain their group alignment");
        for (int y = 4; y < 29; ++y) for (int x = 4; x < 29; ++x) {
            const auto px = 34 + static_cast<int>((x + .5F) * 176 / 33);
            const auto py = 34 + static_cast<int>((y + .5F) * 176 / 33);
            const auto i = (py * 500 + px) * 4;
            const auto expected = (x + y) % 2 ? 0 : 255;
            expect(qr_pixels[i] == expected && qr_pixels[i + 1] == expected
                && qr_pixels[i + 2] == expected && qr_pixels[i + 3] == 255,
                "rounded clipping preserves every QR body module");
        }
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
            const auto full_opacity = target.read_rgba8();
            card.opacity = .5F;
            expect(layer.apply(target.native_render_target(), card, false, changed) && changed
                && target.read_rgba8() != full_opacity && layer.captures() == 1,
                "Cast C fades against its original bounded underlay without recapture");
            expect(layer.apply(target.native_render_target(), card, false, changed) && !changed,
                "constant partial opacity remains static");
            card.visible = false;
            expect(layer.apply(target.native_render_target(), card, false, changed) && changed,
                "offline removes card from held frame");
            expect(target.read_rgba8() == original, "C restores exact underlying program pixels");
        }
        for (const auto& size : {std::pair{3840U, 2160U}, {1280U, 720U}, {960U, 540U}, {1024U, 576U}, {640U, 360U}}) {
            const auto& scaled = renderer.render(device.native_d3d_device(), size.first, size.second, &state, image, true);
            expect(scaled.visible && scaled.x + scaled.width <= size.first
                && scaled.y + scaled.height <= size.second, "bounded output scaling");
            const auto pixels = read_card(scaled, device.native_d3d_device());
            const auto scale = std::min(size.first / 1920.0F, size.second / 1080.0F);
            const auto side = std::round(176 * scale);
            const auto module = side / 33;
            const auto left = std::round(122 * scale - side / 2);
            const auto top = left;
            for (int y = 4; y < 29; ++y) for (int x = 4; x < 29; ++x) {
                const auto px = static_cast<std::size_t>(left + (x + .5F) * module);
                const auto py = static_cast<std::size_t>(top + (y + .5F) * module);
                const auto i = (py * scaled.width + px) * 4;
                expect(pixels[i] == ((x + y) % 2 ? 0 : 255) && pixels[i + 3] == 255,
                    "scaled QR keeps all body modules inside its rounded clip");
            }
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

        {
            HostHudRenderer animated;
            HostHudState content;
            content.options = {true, true, false};
            content.current = {L"当前", L"歌手"};
            content.queue = {{L"一", L""}, {L"二", L""}, {L"三", L""}, {L"四", L""}};
            HostHudPresentation phase{false, 0, 1, 0, true};
            animated.render(device.native_d3d_device(), 1920, 1080, &content, image, true, phase);
            phase.monotonic_seconds = .15;
            auto card = animated.render(device.native_d3d_device(), 1920, 1080, &content, image, true, phase);
            expect(card.visible && !card.animating && animated.rasterizations() == 1, "initial fade reuses one QR card");
            const auto qr_revision = card.revision;
            phase.queue_visible = true; phase.progress = .8F; phase.monotonic_seconds = 1;
            animated.render(device.native_d3d_device(), 1920, 1080, &content, image, true, phase);
            phase.monotonic_seconds = 1.075;
            card = animated.render(device.native_d3d_device(), 1920, 1080, &content, image, true, phase);
            expect(card.opacity > .49F && card.opacity < .51F && card.revision == qr_revision
                && animated.rasterizations() == 1, "fade-out changes opacity, never QR pixels or fonts");
            phase.monotonic_seconds = 1.15;
            animated.render(device.native_d3d_device(), 1920, 1080, &content, image, true, phase);
            phase.monotonic_seconds = 1.225;
            card = animated.render(device.native_d3d_device(), 1920, 1080, &content, image, true, phase);
            expect(card.opacity > .49F && card.opacity < .51F && card.height == 455
                && animated.rasterizations() == 2, "fade-in reuses one newly cached list");
            phase.monotonic_seconds = 1.3;
            animated.render(device.native_d3d_device(), 1920, 1080, &content, image, true, phase);
            content.queue.front().title = L"顶歌后的下一首";
            content.queue.pop_back();
            phase.monotonic_seconds = 1.31;
            card = animated.render(device.native_d3d_device(), 1920, 1080, &content, image, true, phase);
            expect(card.opacity == 1 && !card.animating && card.height == 385
                && animated.rasterizations() == 3, "queue update and resize never restart the tail fade");
            const auto progress = read_card(card, device.native_d3d_device());
            expect(progress[(64 * 500 + 100) * 4] > 200 && progress[(64 * 500 + 432) * 4] < 100,
                "queue mutation preserves the current 80 percent progress, not a new countdown");
            phase.program_instance = 2; phase.progress = 0; phase.monotonic_seconds = 2;
            animated.render(device.native_d3d_device(), 1920, 1080, &content, image, true, phase);
            const auto progress_updates = animated.progress_updates();
            phase.monotonic_seconds = 2.075;
            animated.render(device.native_d3d_device(), 1920, 1080, &content, image, true, phase);
            phase.monotonic_seconds = 2.15;
            card = animated.render(device.native_d3d_device(), 1920, 1080, &content, image, true, phase);
            expect(card.opacity == 1 && !card.animating && animated.rasterizations() == 3
                && animated.progress_updates() == progress_updates,
                "takeover fade neither re-rasterizes text nor reruns the progress stripe");
        }

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
        expect(!host->hud.animation_timer_active, "controller-only mode never starts an animation timer");
        show_host_set_lan_state_callback(handle, nullptr, nullptr);
        show_host_destroy(handle);
    }
    CoUninitialize();
    std::cout << "Audience HUD cache, network gate, scaling and additive FFI passed.\n";
}
