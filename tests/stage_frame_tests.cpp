#include "kirakara/show/stage_frame.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

using kirakara::show::StageFrameHub;
using kirakara::show::StageFrameLease;
using kirakara::show::StageFramePool;
using kirakara::show::StageFrameResource;
using kirakara::show::StageFrameTiming;
using kirakara::show::StageOutputProfile;
using kirakara::show::StageOutputRole;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class MockTexture final : public StageFrameResource {
public:
    explicit MockTexture(std::size_t slot,
            std::shared_ptr<std::size_t> retirement_count = {})
        : slot_(slot), retirement_count_(std::move(retirement_count)) {}

    void* native_resource() noexcept override { return this; }
    std::uintptr_t shared_handle() const noexcept override {
        return static_cast<std::uintptr_t>(slot_ + 1);
    }
    void retire_producer_access() noexcept override {
        if (retirement_count_) ++*retirement_count_;
    }

private:
    std::size_t slot_{};
    std::shared_ptr<std::size_t> retirement_count_;
};

StageFrameLease produce(
        StageFramePool& pool,
        std::uint64_t generation,
        std::uint64_t frame_id) {
    auto write = pool.try_acquire();
    expect(static_cast<bool>(write), "producer acquired a free frame slot");
    expect(write.native_resource() != nullptr, "write lease exposes texture");
    return write.publish(StageFrameTiming{
        generation,
        frame_id,
        static_cast<std::int64_t>(frame_id) * 166667,
        166667,
    });
}

} // namespace

int main() {
    StageFramePool pool;
    const StageOutputProfile profile{
        StageOutputRole::cast, 1920, 1080, 60, 1};
    expect(!pool.configure(
            StageOutputProfile{StageOutputRole::cast, 0, 1080, 60, 1},
            [](const auto&, std::size_t slot) {
                return std::make_shared<MockTexture>(slot);
            }),
        "invalid output profile is rejected");
    expect(pool.configure(profile,
            [](const auto&, std::size_t slot) {
                return std::make_shared<MockTexture>(slot);
            }),
        "triple frame pool configured");
    expect(pool.capacity() == 3 && pool.available() == 3,
        "pool starts with three writable slots");

    StageFramePool rotation_pool;
    expect(rotation_pool.configure(profile,
            [](const auto&, std::size_t slot) {
                return std::make_shared<MockTexture>(slot);
            }),
        "rotation test pool configured");
    std::uintptr_t rotated_handles[4]{};
    for (std::uint64_t id = 0; id < 4; ++id) {
        auto rotated = produce(rotation_pool, 1, id);
        rotated_handles[id] = rotated->shared_handle;
    }
    expect(rotated_handles[0] != rotated_handles[1]
            && rotated_handles[1] != rotated_handles[2]
            && rotated_handles[0] == rotated_handles[3],
        "released frame slots rotate before reuse");

    auto cancelled_write = pool.try_acquire();
    expect(!cancelled_write.publish(StageFrameTiming{1, 0, 0, 0}),
        "invalid frame timing cancels publication");
    expect(pool.available() == 3, "cancelled write immediately returns slot");

    auto first = produce(pool, 1, 1);
    auto second = produce(pool, 1, 2);
    auto third = produce(pool, 1, 3);
    expect(first && second && third, "published leases are valid");
    expect(first->profile.width == 1920 && first->profile.height == 1080,
        "published frame keeps output profile");
    expect(first->shared_handle != 0, "published frame exposes shared handle");
    expect(!pool.try_acquire(), "producer never waits when all slots are leased");
    expect(!pool.configure(
            StageOutputProfile{
                StageOutputRole::physical_display, 2560, 1440, 60, 1},
            [](const auto&, std::size_t slot) {
                return std::make_shared<MockTexture>(slot);
            }),
        "active frame leases prevent output profile reconfiguration");
    expect(pool.profile().width == 1920 && pool.profile().height == 1080,
        "failed reconfiguration preserves current pool");
    second = {};
    expect(pool.available() == 1, "released read lease returns one pool slot");
    auto replacement = produce(pool, 1, 4);
    expect(replacement->timing.frame_id == 4, "released slot is reusable");
    first = {};
    third = {};
    replacement = {};

    StageFrameHub hub;
    auto preview_sink = hub.subscribe();
    auto stage_window_sink = hub.subscribe();
    auto cast_sink = hub.subscribe();
    expect(hub.subscriber_count() == 3, "three mock sinks subscribed");

    auto frame = produce(pool, 2, 1);
    expect(hub.publish(frame) == 3, "first frame reaches every sink");
    auto preview_frame = preview_sink.take_latest();
    auto held_stage_frame = stage_window_sink.take_latest();
    expect(preview_frame->timing.frame_id == 1,
        "preview consumes the first frame");
    expect(held_stage_frame->timing.frame_id == 1,
        "stage sink can retain a frame lease");
    preview_frame = {};
    frame = {};

    // Preview consumes continuously. StageWindow pauses while retaining its
    // first frame. Cast does not consume at all. Their mailboxes must only
    // replace their own stale frames and must not stall the producer.
    for (std::uint64_t id = 2; id <= 6; ++id) {
        frame = produce(pool, 2, id);
        expect(hub.publish(frame) == 3, "paused sink cannot block publication");
        preview_frame = preview_sink.take_latest();
        expect(preview_frame->timing.frame_id == id,
            "preview independently receives latest frame");
        preview_frame = {};
        frame = {};
    }
    expect(stage_window_sink.dropped_frames() == 4,
        "paused stage mailbox drops only its replaced frames");
    expect(cast_sink.dropped_frames() == 5,
        "unconsumed cast mailbox tracks its own drops");
    auto latest_cast = cast_sink.take_latest();
    expect(latest_cast->timing.frame_id == 6,
        "resumed cast sink jumps directly to newest frame");

    // Destroy one sink while another still holds an old frame. Publication to
    // the remaining sinks must continue and stale generations cannot overwrite
    // a newer program watermark, even after the mailbox was consumed.
    stage_window_sink = {};
    expect(hub.subscriber_count() == 2, "destroyed sink is removed from hub");
    latest_cast = {};
    frame = produce(pool, 3, 0);
    expect(hub.publish(frame) == 2, "new generation reaches live sinks");
    auto generation_three = cast_sink.take_latest();
    expect(generation_three->timing.generation == 3,
        "new generation resets frame id ordering");
    frame = {};

    auto stale = produce(pool, 2, 99);
    expect(hub.publish(stale) == 0,
        "old generation cannot replace a consumed newer program");
    expect(!cast_sink.take_latest(), "stale frame leaves mailbox empty");

    held_stage_frame = {};
    generation_three = {};
    stale = {};
    preview_sink = {};
    cast_sink = {};
    expect(pool.available() == pool.capacity(),
        "destroyed and resumed sinks release every frame slot");

    StageFramePool retiring_pool;
    auto retirement_count = std::make_shared<std::size_t>();
    expect(retiring_pool.configure(profile,
            [retirement_count](const auto&, std::size_t slot) {
                return std::make_shared<MockTexture>(
                    slot, retirement_count);
            }),
        "retirement test pool configured");
    auto retiring_frame = produce(retiring_pool, 4, 1);
    retiring_pool.retire_producer_access();
    expect(*retirement_count == retiring_pool.capacity(),
        "pool retires every producer resource exactly once");
    expect(!retiring_pool.configured() && !retiring_pool.try_acquire(),
        "retired pool cannot accept new writes");
    expect(retiring_frame && retiring_frame->shared_handle != 0,
        "retirement preserves outstanding immutable read leases");
    retiring_pool.retire_producer_access();
    expect(*retirement_count == retiring_pool.capacity(),
        "explicit retirement is safe to repeat");
    retiring_frame = {};

    std::cout << "StageFrame pool and latest-frame hub tests passed.\n";
}
