#include "kirakara/show/stage_overlay.hpp"

#include <cstdlib>
#include <iostream>

using namespace kirakara::show;

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    StageOverlayState state;
    expect(state.valid(), "disabled default state is valid");

    state.qr.visible = true;
    expect(!state.valid(), "visible QR requires payload");
    state.qr.payload = L"http://192.0.2.1:7391";
    expect(state.valid(), "QR payload does not require temporary artwork");

    state.announcement.visible = true;
    expect(!state.valid(), "visible announcement requires text");
    state.announcement.text = L"Kira Karaoke";
    expect(state.valid(), "text-only announcement state is valid");

    StageOverlayAsset first{L"qr-frame", L"C:\\cache\\qr.png", 7};
    StageOverlayAsset same{L"qr-frame", L"D:\\moved\\qr.png", 7};
    StageOverlayAsset changed{L"qr-frame", L"D:\\moved\\qr.png", 8};
    expect(first.valid(), "complete asset descriptor is valid");
    expect(first.same_content_as(same),
        "cache identity is independent from local path");
    expect(!first.same_content_as(changed),
        "content revision invalidates the GPU cache entry");

    std::cout << "Stage overlay contract tests passed.\n";
    return 0;
}
