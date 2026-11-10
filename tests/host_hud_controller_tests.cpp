#include "../apps/show_host/overlay/host_hud_controller.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

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
    expect(controller.update(in).queue_visible, "actual takeover starts three seconds");
    near(controller.update(in).progress, 0, "takeover never restarts the ending progress bar");
    in.position = 1; in.monotonic_seconds = 22;
    expect(controller.update(in).queue_visible, "takeover window advances without a progress bar");
    in.advancing = false; in.monotonic_seconds = 50;
    expect(controller.update(in).queue_visible, "pause/buffering freezes presentation");
    near(controller.update(in).progress, 0, "paused takeover leaves the stripe empty");
    in.advancing = true; in.monotonic_seconds = 100;
    expect(controller.update(in).queue_visible, "resume does not charge paused time");
    in.position = 2; in.monotonic_seconds = 101;
    expect(controller.update(in).queue_visible, "resume continues remaining time");
    ++in.discontinuity_revision; in.position = 0; in.monotonic_seconds = 102;
    expect(controller.update(in).queue_visible, "replay does not reset or lengthen timer");
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
    expect(controller.update(in).queue_visible, "new instance, including repeated same song, starts once");
    near(controller.update(in).progress, 0, "next song keeps the completed bar at zero");
    in.taken_over = false; in.position = 1; in.monotonic_seconds = 221;
    expect(controller.update(in).queue_visible, "output reconfiguration does not start a new window");
    in.active = false; in.monotonic_seconds = 222;
    expect(!controller.update(in).queue_visible, "idle restores QR phase");

    HostHudController unknown;
    in = {1, 0, 0, 0, 0, true, true, true}; unknown.update(in);
    in.position = 80; in.monotonic_seconds = 80;
    expect(!unknown.update(in).queue_visible, "unknown duration does not invent a tail");
    in.duration = 100; unknown.update(in);
    in.position = 90; in.monotonic_seconds = 90;
    expect(unknown.update(in).queue_visible, "reliable duration enables future natural crossing");
    in.position = std::numeric_limits<double>::quiet_NaN(); in.monotonic_seconds = 91;
    expect(unknown.update(in).monotonic_seconds == 91,
        "invalid media metadata cannot freeze the separate opacity animation clock");

    HostHudTransition transition;
    HostHudTransitionInput visual{HostHudScene::qr, 1, 0, false};
    near(transition.update(visual).opacity, 1, "non-animated callers preserve the stable card");
    visual = {HostHudScene::queue, 1, 1, true};
    auto view = transition.update(visual);
    expect(view.scene == HostHudScene::qr && view.animating, "cross-scene fade starts on the old card");
    visual.monotonic_seconds = 1.075;
    near(transition.update(visual).opacity, .5F, "old card fades out in 150ms");
    visual.monotonic_seconds = 1.15; view = transition.update(visual);
    expect(view.scene == HostHudScene::queue && view.opacity == 0, "card changes only at zero opacity");
    visual.monotonic_seconds = 1.225;
    near(transition.update(visual).opacity, .5F, "new card fades in over the next 150ms");
    visual.monotonic_seconds = 1.3; view = transition.update(visual);
    expect(view.opacity == 1 && !view.animating, "cross-scene transition ends at 300ms");
    visual.program_instance = 2; visual.monotonic_seconds = 2;
    near(transition.update(visual).opacity, 0, "actual song takeover starts a 150ms list fade-in");
    visual.monotonic_seconds = 2.075;
    near(transition.update(visual).opacity, .5F, "same-scene takeover halfway");
    visual.monotonic_seconds = 2.15;
    expect(!transition.update(visual).animating, "takeover fade stops at 150ms");
    for (int i = 0; i < 100; ++i)
        expect(!transition.update(visual).animating, "unchanged event identity never animates again");
    visual.desired = HostHudScene::qr; visual.monotonic_seconds = 3;
    transition.update(visual);
    visual.monotonic_seconds = 3.15;
    expect(transition.update(visual).scene == HostHudScene::qr, "reverse transition swaps at midpoint");
    visual.monotonic_seconds = 3.3;
    expect(!transition.update(visual).animating, "reverse transition terminates");
    visual.desired = HostHudScene::queue; visual.monotonic_seconds = 4;
    transition.update(visual);
    visual.qr_available = false; visual.monotonic_seconds = 4.01;
    view = transition.update(visual);
    expect(view.scene == HostHudScene::queue && view.opacity == 0,
        "disconnect instantly removes an outgoing QR, not after its fade");
    visual.desired = HostHudScene::hidden;
    visual.queue_available = false;
    expect(transition.update(visual).scene == HostHudScene::hidden
        && !transition.update(visual).animating, "manual disable stops all animation immediately");
    visual = {HostHudScene::queue, 3, 5, false};
    transition.update(visual);
    visual.desired = HostHudScene::hidden; visual.enabled = true;
    expect(transition.update(visual).animating, "list can fade out when the QR option is off");
    visual.monotonic_seconds = 5.15; view = transition.update(visual);
    expect(view.scene == HostHudScene::hidden && !view.animating,
        "list-to-empty fade ends in 150ms without an empty fade-in");
    std::cout << "HUD takeover, seek/replay, pause, tail and idle rules passed.\n";
}
