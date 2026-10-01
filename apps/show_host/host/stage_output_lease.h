#pragma once

#include <atomic>
#include <cstdint>

enum class StageOutputMode : std::uint8_t {
    none,
    physical_display,
    ts_stream,
};

// Owns the single Stage program output slot. Physical presentation and
// encoded streaming are alternate presenters for the same program feed.
class StageOutputLease {
public:
    [[nodiscard]] bool try_acquire(StageOutputMode requested) noexcept {
        if (requested == StageOutputMode::none) return false;
        auto expected = StageOutputMode::none;
        if (mode_.compare_exchange_strong(
                expected, requested, std::memory_order_acq_rel)) {
            return true;
        }
        return expected == requested;
    }

    [[nodiscard]] bool release(StageOutputMode owner) noexcept {
        if (owner == StageOutputMode::none) return false;
        auto expected = owner;
        return mode_.compare_exchange_strong(
            expected, StageOutputMode::none, std::memory_order_acq_rel);
    }

    [[nodiscard]] StageOutputMode mode() const noexcept {
        return mode_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool is_held_by(StageOutputMode owner) const noexcept {
        return owner != StageOutputMode::none && mode() == owner;
    }

private:
    std::atomic<StageOutputMode> mode_{StageOutputMode::none};
};
