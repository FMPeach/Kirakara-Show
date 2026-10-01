#include "../apps/show_host/stage/native_stage_frame_scheduler.h"

#include <cstdlib>
#include <iostream>

namespace {

void expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "native stage frame scheduler test failed: "
              << message << '\n';
    std::exit(1);
}

NativeStageContentIdentity paused_identity() {
    return NativeStageContentIdentity{
        .static_content = true,
        .has_video_frame = true,
        .draw_program_overlays = true,
        .playback_state = 3,
        .program_generation = 9,
        .decoded_generation = 9,
        .decoded_frame_id = 42,
        .source_width = 1920,
        .source_height = 1080,
        .position_100ns = 25'000'000,
        .playback_revision = 7,
        .overlay_revision = 3,
        .output_revision = 2,
        .output_width = 1920,
        .output_height = 1080,
    };
}

}  // namespace

int main() {
    NativeStageFrameScheduler scheduler;
    auto identity = paused_identity();
    expect(scheduler.should_render(identity),
        "first static frame must render");
    scheduler.commit(identity);
    expect(!scheduler.should_render(identity),
        "identical static frame should be retained");
    expect(scheduler.should_render(identity, true),
        "explicit redraw must bypass static retention");

    auto changed = identity;
    ++changed.decoded_frame_id;
    expect(scheduler.should_render(changed),
        "new decoded frame must render");
    changed = identity;
    ++changed.overlay_revision;
    expect(scheduler.should_render(changed),
        "overlay revision must render");
    changed = identity;
    ++changed.output_revision;
    expect(scheduler.should_render(changed),
        "swap-chain revision must render");
    changed = identity;
    ++changed.position_100ns;
    expect(scheduler.should_render(changed),
        "stationary-time change must render");

    auto playing = identity;
    playing.static_content = false;
    scheduler.commit(playing);
    expect(scheduler.should_render(playing),
        "advancing playback must never be suppressed");

    scheduler.commit(identity);
    scheduler.invalidate();
    expect(!scheduler.has_committed_frame(),
        "invalidation must clear committed state");
    expect(scheduler.should_render(identity),
        "invalidated static frame must render again");

    std::cout << "Native Stage frame scheduler tests passed.\n";
    return 0;
}
