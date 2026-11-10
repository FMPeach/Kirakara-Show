#pragma once
#include <cstdint>
#include <mutex>

struct HostHudClockInput {
    std::uint64_t program_instance{};
    std::uint64_t discontinuity_revision{};
    double position{}, duration{}, monotonic_seconds{};
    bool active{}, advancing{}, taken_over{};
};
struct HostHudPresentation {
    bool queue_visible{};
    float progress{1};
};

// Shared, bounded state across output-mode changes. Never controls media clocks.
class HostHudController {
public:
    HostHudPresentation update(const HostHudClockInput& input);
private:
    HostHudPresentation presentation() const noexcept;
    std::mutex mutex_;
    std::uint64_t instance_{}, discontinuity_{};
    double last_now_{}, last_position_{}, last_duration_{};
    double takeover_remaining_{}, tail_remaining_{};
    bool initialized_{}, was_advancing_{}, tail_triggered_{};
};
