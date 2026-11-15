#pragma once
#include <cstdint>
#include <mutex>

struct HostHudClockInput {
    std::uint64_t program_instance{};
    std::uint64_t discontinuity_revision{};
    double position{}, duration{}, monotonic_seconds{};
    bool active{}, advancing{}, taken_over{};
    bool holding_previous_output{};
};
struct HostHudPresentation {
    bool queue_visible{};
    float progress{1};
    std::uint64_t program_instance{};
    double monotonic_seconds{};
    bool transitions_enabled{};
};

enum class HostHudScene { hidden, qr, queue };
struct HostHudTransitionInput {
    HostHudScene desired{HostHudScene::hidden};
    std::uint64_t program_instance{};
    double monotonic_seconds{};
    bool enabled{}, qr_available{true}, queue_available{true};
};
struct HostHudAppearance {
    HostHudScene scene{HostHudScene::hidden};
    float opacity{};
    bool animating{};
};

enum class HostHudAction : std::uint32_t {
    none, play, pause, replay, skip, vocal, accompaniment, volume_up, volume_down,
};

[[nodiscard]] constexpr bool is_host_hud_audio_action(HostHudAction action) noexcept {
    return action >= HostHudAction::vocal && action <= HostHudAction::volume_down;
}

struct HostHudActionEvent {
    HostHudAction action{};
    std::uint64_t sequence{};
    double started{};
    bool pause_active{};
};

struct HostHudActionAppearance {
    HostHudAction action{};
    float opacity{};
    bool animating{};
    // An idle physical output needs only one wake-up before the fade starts.
    double next_deadline{};
};

[[nodiscard]] HostHudActionAppearance evaluate_host_hud_action(
    const HostHudActionEvent* event, double now) noexcept;

// Whole-card opacity follows scene changes. Row-content blending belongs to
// the renderer; metadata updates never restart this transition or a countdown.
class HostHudTransition {
public:
    HostHudAppearance update(const HostHudTransitionInput& input);
private:
    enum class Phase { steady, fade_out, fade_in };
    HostHudAppearance evaluate(double now);
    HostHudScene current_{HostHudScene::hidden}, target_{HostHudScene::hidden};
    Phase phase_{Phase::steady};
    double started_{};
    float from_opacity_{};
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
