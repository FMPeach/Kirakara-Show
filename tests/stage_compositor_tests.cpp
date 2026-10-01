#include "kirakara/show/win32/stage_compositor.hpp"

#include <d2d1.h>

#include <cstdlib>
#include <iostream>
#include <vector>

using kirakara::show::win32::PixelRect;
using kirakara::show::win32::StageCanvas;
using kirakara::show::win32::StageCompositor;

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void expect_rect(PixelRect actual, PixelRect expected, const char* message) {
    if (actual.x != expected.x || actual.y != expected.y
            || actual.width != expected.width
            || actual.height != expected.height) {
        std::cerr << "FAIL: " << message << " got "
                  << actual.x << ',' << actual.y << ' '
                  << actual.width << 'x' << actual.height << " expected "
                  << expected.x << ',' << expected.y << ' '
                  << expected.width << 'x' << expected.height << '\n';
        std::exit(EXIT_FAILURE);
    }
}

// Build a solid-color BGRA8 frame.
std::vector<std::uint8_t> solid_bgra(
    std::uint32_t w, std::uint32_t h,
    std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4U);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i]     = b;
        pixels[i + 1] = g;
        pixels[i + 2] = r;
        pixels[i + 3] = a;
    }
    return pixels;
}

} // namespace

int main() {
    const StageCanvas canvas{};
    expect(canvas.width == 1920 && canvas.height == 1080,
        "default canvas is 1080p");
    expect(canvas.frame_rate_num == 60 && canvas.frame_rate_den == 1,
        "default canvas is 60 fps");

    expect_rect(StageCompositor::letterbox_rect(1280, 720, canvas),
        PixelRect{0, 0, 1920, 1080}, "16:9 source fills 1080p canvas");
    expect_rect(StageCompositor::letterbox_rect(3840, 2160, canvas),
        PixelRect{0, 0, 1920, 1080}, "4K source scales to canvas");
    expect_rect(StageCompositor::letterbox_rect(720, 480, canvas),
        PixelRect{150, 0, 1620, 1080}, "3:2 source gets pillarbox");
    expect_rect(StageCompositor::letterbox_rect(1080, 1920, canvas),
        PixelRect{656, 0, 608, 1080}, "vertical source gets pillarbox");
    expect_rect(StageCompositor::letterbox_rect(0, 0, canvas),
        PixelRect{0, 0, 1920, 1080}, "idle source covers canvas");

    StageCompositor compositor;
    expect(!compositor.configure(StageCanvas{0, 1080, 60, 1}),
        "invalid canvas rejected");
    expect(compositor.configure(canvas), "default compositor canvas");
    expect(compositor.begin_frame(720, 480), "begin compositor frame");
    const auto frame = compositor.current_frame();
    expect(frame.canvas.width == 1920 && frame.canvas.height == 1080,
        "frame keeps fixed canvas");
    expect_rect(frame.video_rect, PixelRect{150, 0, 1620, 1080},
        "frame stores source letterbox rect");
    expect(frame.overlay_render_target != nullptr, "overlay D2D target");
    expect(frame.overlay_texture != nullptr, "overlay D3D texture");
    expect(frame.overlay_dxgi_surface != nullptr, "overlay DXGI surface");
    expect(frame.d3d_device != nullptr, "overlay D3D device");

    // Overlay starts transparent.
    auto pixels = compositor.overlay_surface().read_rgba8();
    expect(pixels.size() == 1920U * 1080U * 4U, "overlay readback size");
    expect(pixels[3] == 0, "transparent overlay alpha at origin");
    expect(pixels[pixels.size() - 1] == 0, "transparent overlay alpha at end");

    // Video composite: 320x240 green frame letterboxed into 1920x1080.
    // 320:240 = 4:3 → scale = min(1920/320, 1080/240) = min(6, 4.5) = 4.5
    // output: 1440x1080, x = (1920-1440)/2 = 240
    const auto green = solid_bgra(320, 240, 0, 255, 0);
    expect(compositor.begin_frame(320, 240), "begin frame for video test");
    expect(compositor.upload_video_bgra(
        green.data(), 320, 240), "upload video pixels");
    pixels = compositor.overlay_surface().read_rgba8();
    // Pixel at (240, 0) should be inside video rect — green.
    const auto idx = (240U * 4U);  // x=240, y=0
    // solid_bgra(r=0,g=255,b=0) → BGRA[0,255,0,255] → read_rgba8 RGBA[0,255,0,255]
    expect(pixels[idx] == 0 && pixels[idx + 1] == 255
           && pixels[idx + 2] == 0 && pixels[idx + 3] == 255,
        "green video at left edge");
    const auto bottom_right_inside =
        ((1079U * 1920U) + 1679U) * 4U;
    expect(pixels[bottom_right_inside] == 0
           && pixels[bottom_right_inside + 1] == 255
           && pixels[bottom_right_inside + 2] == 0
           && pixels[bottom_right_inside + 3] == 255,
        "green video at scaled bottom-right edge");
    const auto right_pillarbox =
        ((1079U * 1920U) + 1680U) * 4U;
    expect(pixels[right_pillarbox] == 0
           && pixels[right_pillarbox + 1] == 0
           && pixels[right_pillarbox + 2] == 0
           && pixels[right_pillarbox + 3] == 0,
        "transparent pillarbox right of scaled video");
    // Pixel at (0, 0) should be outside video rect — not green (pillarbox).
    // After clear_overlay, it should be transparent black.
    expect(pixels[0] == 0 && pixels[1] == 0
           && pixels[2] == 0 && pixels[3] == 0,
        "transparent pillarbox left of video");

    // Composite again with a different source: full-canvas video.
    // Simulate an overlay renderer leaving its 1280x720 -> 1080p transform
    // attached to the shared target.  The compositor must reset it before
    // drawing the next video frame.
    auto* render_target = static_cast<ID2D1RenderTarget*>(
        compositor.current_frame().overlay_render_target);
    render_target->SetTransform(D2D1::Matrix3x2F::Scale(1.5F, 1.5F));
    const auto red = solid_bgra(1920, 1080, 255, 0, 0);
    expect(compositor.begin_frame(1920, 1080), "begin frame 1080p");
    expect(compositor.upload_video_bgra(
        red.data(), 1920, 1080), "upload full canvas video");
    pixels = compositor.overlay_surface().read_rgba8();
    // solid_bgra(r=255,g=0,b=0) → BGRA[0,0,255,255] → read_rgba8 RGBA[255,0,0,255]
    expect(pixels[0] == 255 && pixels[1] == 0
           && pixels[2] == 0 && pixels[3] == 255,
        "full canvas video covers top-left");
    const auto full_canvas_bottom_right =
        ((1079U * 1920U) + 1919U) * 4U;
    expect(pixels[full_canvas_bottom_right] == 255
           && pixels[full_canvas_bottom_right + 1] == 0
           && pixels[full_canvas_bottom_right + 2] == 0
           && pixels[full_canvas_bottom_right + 3] == 255,
        "overlay transform cannot crop next video frame");

    std::cout << "StageCompositor fixed-canvas test passed.\n";
}
