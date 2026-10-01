#include "kirakara/show/win32/offscreen_surface.hpp"

#include <cstdlib>
#include <iostream>
#include <filesystem>

using kirakara::show::Color;
using kirakara::show::win32::OffscreenSurface;

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool near_byte(std::uint8_t value, int expected) {
    return std::abs(static_cast<int>(value) - expected) <= 1;
}

} // namespace

int main() {
    OffscreenSurface surface;
    expect(surface.resize(2, 2), "surface creation");
    expect(surface.width() == 2 && surface.height() == 2, "surface size");
    surface.begin_draw();
    surface.clear(Color{1.0F, 0.0F, 0.0F, 0.5F});
    expect(surface.end_draw(), "end draw");
    const auto pixels = surface.read_rgba8();
    expect(pixels.size() == 16, "RGBA8 byte count");
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        expect(near_byte(pixels[i + 0], 128), "premultiplied red");
        expect(near_byte(pixels[i + 1], 0), "green channel");
        expect(near_byte(pixels[i + 2], 0), "blue channel");
        expect(near_byte(pixels[i + 3], 128), "alpha channel");
    }
    const auto png = std::filesystem::current_path() / "offscreen-surface-test.png";
    expect(surface.save_png(png.wstring()), "PNG encoding");
    expect(std::filesystem::exists(png)
        && std::filesystem::file_size(png) > 0, "PNG file output");
    std::filesystem::remove(png);
    std::cout << "Kirakara offscreen RGBA8 test passed.\n";
}
