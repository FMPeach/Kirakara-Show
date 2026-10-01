#include "kirakara/show/song_title.hpp"
#include "kirakara/show/config.hpp"
#include "kirakara/show/win32/offscreen_surface.hpp"
#include "kirakara/show/win32/song_title_renderer.hpp"

#include <windows.h>

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

using namespace kirakara::show;

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void clear(win32::OffscreenSurface& surface) {
    surface.begin_draw();
    surface.clear(Color{0, 0, 0, 0});
    expect(surface.end_draw(), "surface clear");
}

} // namespace

int main() {
    expect(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)),
        "COM initialization");
    {
        win32::OffscreenSurface surface;
        expect(surface.resize(1280, 720), "offscreen surface");
        win32::SongTitleRenderer renderer;
        expect(renderer.initialize(), "title renderer initialization");
        expect(renderer.attach_native_target(surface.native_render_target()),
            "title target attachment");

        auto config = default_song_title_config();
        config.enabled = true;
        config.duration = 5.0;
        config.text_fade = false;
        SongTitleBlock block;
        block.lines = {"TITLE"};
        block.style.font_family = L"Arial";
        block.style.font_families = {L"Arial"};
        block.style.y = 180.0F;
        config.blocks.push_back(std::move(block));

        surface.begin_draw();
        surface.clear(Color{0.2F, 0.3F, 0.4F, 1.0F});
        expect(surface.end_draw(), "opaque program background");
        expect(renderer.render(config, 1.0, 0.666),
            "title overlays an existing program frame");
        const auto composited_pixels = surface.read_rgba8();
        expect(composited_pixels.size() >= 4
                && composited_pixels[3] == 255,
            "title renderer does not clear the program background");

        clear(surface);
        expect(renderer.render(config, 1.0, 0.666), "title frame render");
        const auto title_pixels = surface.read_rgba8();
        std::size_t title_visible{};
        for (std::size_t index = 0; index < title_pixels.size(); index += 4) {
            if (title_pixels[index + 3] > 0) ++title_visible;
        }
        expect(title_visible > 1000, "title produced visible pixels");

        clear(surface);
        expect(renderer.render(config, 0.0, 0.666),
            "title frame at media zero");
        const auto overlay_pixels = surface.read_rgba8();
        std::size_t overlay_visible{};
        for (std::size_t index = 0; index < overlay_pixels.size(); index += 4) {
            if (overlay_pixels[index + 3] > 0) ++overlay_visible;
        }
        expect(overlay_visible > 1000,
            "title starts at media zero");

        clear(surface);
        expect(renderer.render(config, -2.0, 0.666),
            "negative-time title frame render");
        const auto parked_pixels = surface.read_rgba8();
        std::size_t parked_visible{};
        for (std::size_t index = 0; index < parked_pixels.size(); index += 4) {
            if (parked_pixels[index + 3] > 0) ++parked_visible;
        }
        expect(parked_visible == 0,
            "negative media time leaves title transparent");

        auto embedded = default_app_config();
        expect(load_app_config_json(R"({
            "songTitle": {
                "enabled": true,
                "durationSec": 5,
                "textFade": false,
                "prelude": {
                    "enabled": true,
                    "backgroundImage": {
                        "name": "red.bmp",
                        "mime": "image/bmp",
                        "base64": "Qk06AAAAAAAAADYAAAAoAAAAAQAAAAEAAAABABgAAAAAAAQAAAATCwAAEwsAAAAAAAAAAAAAAAD/AA=="
                    }
                },
                "groups": []
            }
        })", embedded), "embedded image config parse");
        expect(embedded.song_title.blocks.empty(),
            "editor-only prelude does not enter the renderer model");
        clear(surface);
        expect(renderer.render(embedded.song_title, 0.0, 0.666),
            "empty normalized title render");
        const auto image_pixels = surface.read_rgba8();
        std::size_t image_visible{};
        for (std::size_t index = 0; index < image_pixels.size(); index += 4) {
            if (image_pixels[index + 3] > 0) ++image_visible;
        }
        expect(image_visible == 0,
            "discarded editor metadata cannot draw pixels");

        auto empty = default_song_title_config();
        empty.enabled = true;
        empty.text_fade = false;
        clear(surface);
        expect(renderer.render(empty, 1.0, 0.666),
            "enabled empty title render is valid");
        const auto empty_pixels = surface.read_rgba8();
        std::size_t empty_visible{};
        for (std::size_t index = 0; index < empty_pixels.size(); index += 4) {
            if (empty_pixels[index + 3] > 0) ++empty_visible;
        }
        expect(empty_visible == 0,
            "enabled empty title renderer is a drawing no-op");
        renderer.shutdown();
    }
    CoUninitialize();
    std::cout << "Song title renderer tests passed.\n";
}
