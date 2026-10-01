#include "kirakara/show/win32/stage_window_presenter.hpp"
#include "kirakara/show/win32/unified_stage_renderer.hpp"

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

LRESULT CALLBACK presenter_test_proc(
        HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

kirakara::show::StageFrameLease render_frame(
        kirakara::show::win32::UnifiedStageRenderer& renderer,
        kirakara::show::StageFrameMailbox& mailbox,
        std::uint64_t generation,
        std::uint64_t frame_id) {
    const auto result = renderer.render(
        kirakara::show::win32::UnifiedStageRenderRequest{
            .timing = kirakara::show::StageFrameTiming{
                generation, frame_id, 0, 166667},
            .project_time = 0.0,
            .idle_color = kirakara::show::Color{
                12.0F / 255.0F,
                16.0F / 255.0F,
                24.0F / 255.0F,
                1.0F},
        });
    expect(result == kirakara::show::win32::UnifiedStageRenderResult::published,
        "Unified Stage frame should publish");
    auto frame = mailbox.take_latest();
    expect(static_cast<bool>(frame), "mailbox should contain the frame");
    return frame;
}

} // namespace

int main() {
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    expect(SUCCEEDED(com), "COM should initialize for Direct2D");
    const auto instance = GetModuleHandleW(nullptr);
    constexpr wchar_t window_class[] = L"KirakaraStagePresenterTest";
    WNDCLASSW definition{};
    definition.hInstance = instance;
    definition.lpfnWndProc = presenter_test_proc;
    definition.lpszClassName = window_class;
    expect(RegisterClassW(&definition) != 0
            || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
        "test window class should register");

    const auto window = CreateWindowExW(
        WS_EX_NOACTIVATE,
        window_class,
        L"Stage presenter test",
        WS_POPUP,
        -10000,
        -10000,
        640,
        360,
        nullptr,
        nullptr,
        instance,
        nullptr);
    expect(window != nullptr, "test HWND should be created");

    kirakara::show::win32::UnifiedStageRenderer renderer;
    kirakara::show::win32::StageWindowPresenter presenter;
    const std::array profiles{
        kirakara::show::StageOutputProfile{
            kirakara::show::StageOutputRole::physical_display,
            1920, 1080, 60, 1,
            kirakara::show::StagePixelFormat::bgra8},
        kirakara::show::StageOutputProfile{
            kirakara::show::StageOutputRole::physical_display,
            2560, 1440, 60, 1,
            kirakara::show::StagePixelFormat::bgra8},
        kirakara::show::StageOutputProfile{
            kirakara::show::StageOutputRole::physical_display,
            3840, 2160, 60, 1,
            kirakara::show::StagePixelFormat::bgra8},
    };

    std::uint64_t generation = 1;
    for (const auto& profile : profiles) {
        presenter.release_frame();
        expect(renderer.configure(profile),
            "renderer should accept native monitor profile");
        auto mailbox = renderer.subscribe();
        if (!presenter.initialized()) {
            expect(presenter.initialize(
                    window, renderer.native_d3d_device()),
                "presenter should initialize on the Stage device");
        }
        expect(presenter.resize(profile.width, profile.height),
            "swap chain should follow native monitor pixels");
        auto frame = render_frame(renderer, mailbox, generation, 0);
        expect(presenter.present(frame, false),
            "native-size Stage frame should present");
        expect(presenter.width() == profile.width
                && presenter.height() == profile.height,
            "presenter size should match physical profile");
        expect(presenter.last_timing().generation == generation,
            "presenter should retain the latest frame timing");
        ++generation;
    }

    presenter.release_frame();
    const kirakara::show::StageOutputProfile preview_profile{
        kirakara::show::StageOutputRole::controller_preview,
        1920, 1080, 60, 1,
        kirakara::show::StagePixelFormat::bgra8};
    expect(renderer.configure(preview_profile),
        "single-screen Stage should remain fixed at 1080p");
    auto preview_mailbox = renderer.subscribe();
    const auto revision_before_preview_resize = presenter.output_revision();
    expect(presenter.resize(960, 540),
        "presenter should resize to the controller preview client area");
    expect(presenter.output_revision() > revision_before_preview_resize,
        "swap-chain resize should advance output revision");
    const auto stable_output_revision = presenter.output_revision();
    expect(presenter.resize(960, 540)
            && presenter.output_revision() == stable_output_revision,
        "same-size resize should retain output revision");
    auto preview_frame = render_frame(
        renderer, preview_mailbox, generation, 0);
    expect(presenter.present(preview_frame, false),
        "1080p Stage frame should GPU-scale into the controller preview");
    expect(presenter.repeat_last(false),
        "presenter should repeat a stable paused frame");
    const auto revision_before_recovery = presenter.output_revision();
    expect(presenter.recover_output(),
        "presenter should rebuild its swap chain without dropping the frame");
    expect(presenter.output_revision() > revision_before_recovery,
        "output recovery should advance output revision");
    expect(presenter.repeat_last(false),
        "presenter should retain the last frame across output recovery");
    expect(presenter.present_black(false),
        "presenter should provide a deterministic idle frame");
    expect(presenter.presented_frames() >= profiles.size() + 4,
        "every requested frame should reach the swap chain");

    presenter.shutdown();
    DestroyWindow(window);
    UnregisterClassW(window_class, instance);
    std::cout << "StageWindowPresenter tests passed\n";
    return 0;
}
