#include "../apps/show_host/overlay/host_hud_controller.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

void expect(bool value, const char* message) {
    if (value) return;
    std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE);
}
void near(float value, float target, const char* message) { expect(std::abs(value - target) < .001F, message); }
int main() {
    HostHudController controller;
    HostHudClockInput in{1, 0, 0, 100, 0, true, true, false};
    expect(!controller.update(in).queue_visible, "request alone never starts takeover timer");
    in.monotonic_seconds = 20;
    expect(!controller.update(in).queue_visible, "preload/cold opening does not consume timer");
    in.taken_over = true; in.monotonic_seconds = 21;
    near(controller.update(in).progress, 1, "actual takeover starts three seconds");
    in.position = 1; in.monotonic_seconds = 22;
    near(controller.update(in).progress, 2.0F / 3, "takeover timer advances");
    in.advancing = false; in.monotonic_seconds = 50;
    near(controller.update(in).progress, 2.0F / 3, "pause/buffering freezes presentation");
    in.advancing = true; in.monotonic_seconds = 100;
    near(controller.update(in).progress, 2.0F / 3, "resume does not charge paused time");
    in.position = 2; in.monotonic_seconds = 101;
    near(controller.update(in).progress, 1.0F / 3, "resume continues remaining time");
    ++in.discontinuity_revision; in.position = 0; in.monotonic_seconds = 102;
    near(controller.update(in).progress, 1.0F / 3, "replay does not reset or lengthen timer");
    in.position = 1; in.monotonic_seconds = 103;
    expect(!controller.update(in).queue_visible, "takeover window completes");
    ++in.discontinuity_revision; in.position = 94; in.monotonic_seconds = 104;
    expect(!controller.update(in).queue_visible, "seek across end boundary never opens list");
    in.position = 95; in.monotonic_seconds = 105;
    expect(!controller.update(in).queue_visible, "seek establishes new baseline");
    ++in.discontinuity_revision; in.position = 0; in.monotonic_seconds = 106;
    controller.update(in);
    in.position = 89; in.monotonic_seconds = 195; controller.update(in);
    in.position = 90; in.monotonic_seconds = 196;
    expect(controller.update(in).queue_visible, "natural final-ten-second crossing opens list");
    ++in.discontinuity_revision; in.position = 0; in.monotonic_seconds = 197;
    near(controller.update(in).progress, 1, "seek in tail phase preserves progress");
    in.position = 1; in.monotonic_seconds = 198;
    near(controller.update(in).progress, .9F, "tail progress continues independently of seek");
    in.position = 10; in.monotonic_seconds = 207;
    expect(controller.update(in).queue_visible, "tail remains visible until a real switch/idle");
    in.program_instance = 2; in.position = 0; in.taken_over = false; in.monotonic_seconds = 210;
    near(controller.update(in).progress, 0, "new loading program retains old phase");
    in.taken_over = true; in.monotonic_seconds = 220;
    near(controller.update(in).progress, 1, "new instance, including repeated same song, starts once");
    in.taken_over = false; in.position = 1; in.monotonic_seconds = 221;
    near(controller.update(in).progress, 2.0F / 3, "output reconfiguration does not start a new window");
    in.active = false; in.monotonic_seconds = 222;
    expect(!controller.update(in).queue_visible, "idle restores QR phase");

    HostHudController unknown;
    in = {1, 0, 0, 0, 0, true, true, true}; unknown.update(in);
    in.position = 80; in.monotonic_seconds = 80;
    expect(!unknown.update(in).queue_visible, "unknown duration does not invent a tail");
    in.duration = 100; unknown.update(in);
    in.position = 90; in.monotonic_seconds = 90;
    expect(unknown.update(in).queue_visible, "reliable duration enables future natural crossing");
    std::cout << "HUD takeover, seek/replay, pause, tail and idle rules passed.\n";
}
