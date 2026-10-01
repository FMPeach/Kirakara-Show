#include "../apps/show_host/stage/stage_texture_publication_cadence.h"
#include "../apps/show_host/stage/native_stage_preview_load_policy.h"

#include <chrono>
#include <cstdlib>
#include <iostream>

namespace {

void expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "stage texture cadence test failed: " << message << '\n';
    std::exit(1);
}

}  // namespace

int main() {
    using namespace std::chrono_literals;
    using Clock = StageTexturePublicationCadence::Clock;

    StageTexturePublicationCadence cadence;
    const Clock::time_point start{1s};

    expect(cadence.should_publish(false, start),
        "single-screen preview should publish immediately");
    expect(cadence.should_publish(false, start + 1ms),
        "single-screen preview should remain unthrottled");

    expect(cadence.should_publish(true, start + 2ms),
        "entering physical Stage mode should publish immediately");
    expect(!cadence.should_publish(true, start + 18ms),
        "physical Stage preview should skip the intermediate 60 Hz tick");
    expect(cadence.should_publish(true, start + 36ms),
        "physical Stage preview should publish at about 30 Hz");
    expect(!cadence.should_publish(true, start + 50ms),
        "physical Stage preview should not publish twice per interval");

    expect(cadence.should_publish(true, start + 2s),
        "a late consumer should publish the latest frame once");
    expect(!cadence.should_publish(true, start + 2s + 1ms),
        "a late consumer should not catch up missed publications");

    expect(cadence.should_publish(false, start + 3s),
        "leaving physical Stage mode should restore immediate publication");
    expect(cadence.should_publish(true, start + 3s + 1ms),
        "re-entering physical Stage mode should not retain an old deadline");

    cadence.reset();
    expect(cadence.should_publish(true, start + 4s),
        "an explicitly reset source should publish immediately");

    const auto interval =
        StageTexturePublicationCadence::dual_screen_preview_interval();
    expect(interval >= 33ms && interval <= 34ms,
        "dual-screen preview interval should be approximately 30 Hz");

    cadence.reset();
    expect(cadence.should_publish(true, 20, start + 5s),
        "entering 20 Hz adaptive cadence should publish immediately");
    expect(!cadence.should_publish(true, 20, start + 5s + 36ms),
        "20 Hz adaptive cadence published too early");
    expect(cadence.should_publish(true, 20, start + 5s + 51ms),
        "20 Hz adaptive cadence did not publish on time");
    expect(cadence.should_publish(true, 15, start + 5s + 52ms),
        "a cadence-level change should publish the latest frame immediately");
    expect(!cadence.should_publish(true, 15, start + 5s + 100ms),
        "15 Hz adaptive cadence published too early");
    expect(cadence.should_publish(true, 15, start + 5s + 119ms),
        "15 Hz adaptive cadence did not publish on time");

    NativeStagePreviewLoadPolicy policy;
    const auto policy_start = Clock::time_point{10s};
    policy.observe(true, policy_start, 2);
    policy.observe(true, policy_start + 700ms, 2);
    policy.observe(true, policy_start + 1400ms, 2);
    policy.observe(true, policy_start + 2100ms, 0);
    expect(policy.level()
            == NativeStagePreviewLoadLevel::reduced_20_fps,
        "sustained misses should reduce Preview to 20 Hz");
    expect(policy.preview_fps() == 20,
        "reduced Preview level reported the wrong cadence");

    policy.observe(true, policy_start + 2200ms, 2);
    policy.observe(true, policy_start + 2900ms, 2);
    policy.observe(true, policy_start + 3600ms, 2);
    policy.observe(true, policy_start + 4300ms, 0);
    expect(policy.level()
            == NativeStagePreviewLoadLevel::minimum_15_fps,
        "continued misses should reduce Preview to 15 Hz");
    policy.observe(true, policy_start + 16s, 0);
    expect(policy.level()
            == NativeStagePreviewLoadLevel::minimum_15_fps,
        "Preview recovered before the stable hysteresis interval");
    policy.observe(true, policy_start + 17s, 0);
    expect(policy.level()
            == NativeStagePreviewLoadLevel::reduced_20_fps,
        "stable Preview should recover one level at a time");
    policy.observe(true, policy_start + 30s, 0);
    expect(policy.level()
            == NativeStagePreviewLoadLevel::nominal_30_fps,
        "Preview should return to 30 Hz after another stable interval");

    policy.reset();
    policy.observe(true, policy_start, 20);
    policy.observe(true, policy_start + 2100ms, 0);
    expect(policy.level()
            == NativeStagePreviewLoadLevel::nominal_30_fps,
        "one severe transient miss event should not reduce Preview cadence");
    policy.observe(false, policy_start + 3s, 20);
    expect(policy.level()
            == NativeStagePreviewLoadLevel::nominal_30_fps,
        "leaving physical Stage mode should reset adaptive pressure");

    std::cout << "Stage texture publication cadence tests passed.\n";
    return 0;
}
