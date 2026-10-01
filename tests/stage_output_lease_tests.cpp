#include "../apps/show_host/host/stage_output_lease.h"

#include <cstdlib>
#include <iostream>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    StageOutputLease lease;
    expect(lease.mode() == StageOutputMode::none,
        "lease should start unowned");
    expect(!lease.try_acquire(StageOutputMode::none),
        "none is not a valid lease owner");

    expect(lease.try_acquire(StageOutputMode::physical_display),
        "physical display should acquire an empty lease");
    expect(lease.try_acquire(StageOutputMode::physical_display),
        "reacquiring the same mode should be idempotent");
    expect(!lease.try_acquire(StageOutputMode::ts_stream),
        "TS stream must not acquire the physical display lease");
    expect(!lease.release(StageOutputMode::ts_stream),
        "a different mode must not release the lease");
    expect(lease.is_held_by(StageOutputMode::physical_display),
        "failed acquisition and release must preserve the owner");

    expect(lease.release(StageOutputMode::physical_display),
        "physical display should release its lease");
    expect(lease.mode() == StageOutputMode::none,
        "released lease should become empty");

    expect(lease.try_acquire(StageOutputMode::ts_stream),
        "TS stream should acquire an empty lease");
    expect(!lease.try_acquire(StageOutputMode::physical_display),
        "physical display must not acquire the TS stream lease");
    expect(lease.release(StageOutputMode::ts_stream),
        "TS stream should release its lease");
    expect(!lease.release(StageOutputMode::ts_stream),
        "releasing an empty lease should be harmless and report false");
    return 0;
}
