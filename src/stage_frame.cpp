#include "kirakara/show/stage_frame.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace kirakara::show {
namespace detail {

struct StageFramePoolSlot {
    std::shared_ptr<StageFrameResource> resource;
    StageFrame frame;
    std::atomic<bool> write_claimed{false};
    std::atomic<std::size_t> active_read_lease{0};
};

struct StageFramePoolState {
    mutable std::mutex slots_mutex;
    StageOutputProfile profile;
    std::vector<std::shared_ptr<StageFramePoolSlot>> slots;
    std::size_t next_slot{};
    bool configured{};
};

struct StageFrameLeaseState {
    explicit StageFrameLeaseState(std::shared_ptr<StageFramePoolSlot> value)
        : slot(std::move(value)) {
        slot->active_read_lease.fetch_add(1, std::memory_order_release);
    }

    ~StageFrameLeaseState() {
        slot->active_read_lease.fetch_sub(1, std::memory_order_release);
    }

    std::shared_ptr<StageFramePoolSlot> slot;
};

struct StageFrameMailboxState {
    std::atomic<std::shared_ptr<const StageFrameLeaseState>> latest;
    std::atomic<std::uint64_t> dropped_frames{};
    std::uint64_t last_generation{};
    std::uint64_t last_frame_id{};
    bool has_watermark{};

    [[nodiscard]] bool offer(
            const std::shared_ptr<const StageFrameLeaseState>& candidate) {
        if (!candidate) return false;
        const auto& timing = candidate->slot->frame.timing;
        if (has_watermark
                && (timing.generation < last_generation
                || (timing.generation == last_generation
                    && timing.frame_id <= last_frame_id))) {
            return false;
        }

        auto previous = latest.exchange(candidate, std::memory_order_acq_rel);
        if (previous) {
            dropped_frames.fetch_add(1, std::memory_order_relaxed);
        }
        last_generation = timing.generation;
        last_frame_id = timing.frame_id;
        has_watermark = true;
        return true;
    }
};

} // namespace detail

bool StageOutputProfile::valid() const noexcept {
    return width > 0 && height > 0
        && refresh_rate_num > 0 && refresh_rate_den > 0;
}

StageFrameLease::StageFrameLease(
        std::shared_ptr<const detail::StageFrameLeaseState> state)
    : state_(std::move(state)) {}

StageFrameLease::operator bool() const noexcept {
    return state_ && state_->slot;
}

const StageFrame* StageFrameLease::get() const noexcept {
    return *this ? &state_->slot->frame : nullptr;
}

const StageFrame* StageFrameLease::operator->() const noexcept {
    return get();
}

struct StageFrameWriteLease::Impl {
    std::shared_ptr<detail::StageFramePoolState> pool;
    std::shared_ptr<detail::StageFramePoolSlot> slot;

    ~Impl() {
        if (slot) {
            slot->write_claimed.store(false, std::memory_order_release);
        }
    }
};

StageFrameWriteLease::StageFrameWriteLease(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

StageFrameWriteLease::~StageFrameWriteLease() = default;
StageFrameWriteLease::StageFrameWriteLease(StageFrameWriteLease&&) noexcept = default;
StageFrameWriteLease& StageFrameWriteLease::operator=(
    StageFrameWriteLease&&) noexcept = default;

StageFrameWriteLease::operator bool() const noexcept {
    return impl_ && impl_->slot && impl_->slot->resource;
}

StageFrameResource* StageFrameWriteLease::resource() noexcept {
    return *this ? impl_->slot->resource.get() : nullptr;
}

void* StageFrameWriteLease::native_resource() noexcept {
    return *this ? impl_->slot->resource->native_resource() : nullptr;
}

std::uintptr_t StageFrameWriteLease::shared_handle() const noexcept {
    return *this ? impl_->slot->resource->shared_handle() : 0;
}

StageOutputProfile StageFrameWriteLease::profile() const noexcept {
    return *this ? impl_->slot->frame.profile : StageOutputProfile{};
}

StageFrameLease StageFrameWriteLease::publish(StageFrameTiming timing) {
    if (!*this) return {};
    if (timing.duration_100ns <= 0) {
        impl_.reset();
        return {};
    }

    auto& slot = *impl_->slot;
    slot.frame.timing = timing;
    slot.frame.native_resource = slot.resource->native_resource();
    slot.frame.shared_handle = slot.resource->shared_handle();

    auto state = std::make_shared<detail::StageFrameLeaseState>(impl_->slot);
    slot.write_claimed.store(false, std::memory_order_release);
    impl_->slot.reset();
    impl_.reset();
    return StageFrameLease(std::move(state));
}

StageFramePool::StageFramePool()
    : state_(std::make_shared<detail::StageFramePoolState>()) {}

StageFramePool::~StageFramePool() = default;
StageFramePool::StageFramePool(StageFramePool&&) noexcept = default;
StageFramePool& StageFramePool::operator=(StageFramePool&&) noexcept = default;

bool StageFramePool::configure(
        StageOutputProfile profile,
        StageFrameResourceFactory resource_factory,
        std::size_t capacity) {
    if (!state_ || !profile.valid() || !resource_factory || capacity == 0) {
        return false;
    }

    std::vector<std::shared_ptr<detail::StageFramePoolSlot>> replacement;
    replacement.reserve(capacity);
    try {
        for (std::size_t index = 0; index < capacity; ++index) {
            auto resource = resource_factory(profile, index);
            if (!resource || !resource->native_resource()) return false;
            auto slot = std::make_shared<detail::StageFramePoolSlot>();
            slot->resource = std::move(resource);
            slot->frame.profile = profile;
            slot->frame.native_resource = slot->resource->native_resource();
            slot->frame.shared_handle = slot->resource->shared_handle();
            replacement.push_back(std::move(slot));
        }
    } catch (...) {
        return false;
    }

    std::lock_guard lock(state_->slots_mutex);
    const auto busy = std::ranges::any_of(state_->slots, [](const auto& slot) {
        return slot->write_claimed.load(std::memory_order_acquire)
            || slot->active_read_lease.load(std::memory_order_acquire) != 0;
    });
    if (busy) return false;
    state_->profile = profile;
    state_->slots = std::move(replacement);
    state_->next_slot = 0;
    state_->configured = true;
    return true;
}

bool StageFramePool::configured() const noexcept {
    if (!state_) return false;
    std::lock_guard lock(state_->slots_mutex);
    return state_->configured;
}

StageOutputProfile StageFramePool::profile() const noexcept {
    if (!state_) return {};
    std::lock_guard lock(state_->slots_mutex);
    return state_->profile;
}

std::size_t StageFramePool::capacity() const noexcept {
    if (!state_) return 0;
    std::lock_guard lock(state_->slots_mutex);
    return state_->slots.size();
}

std::size_t StageFramePool::available() const noexcept {
    if (!state_) return 0;
    std::lock_guard lock(state_->slots_mutex);
    return static_cast<std::size_t>(std::ranges::count_if(
        state_->slots, [](const auto& slot) {
            return !slot->write_claimed.load(std::memory_order_acquire)
                && slot->active_read_lease.load(std::memory_order_acquire) == 0;
        }));
}

void StageFramePool::retire_producer_access() noexcept {
    if (!state_) return;
    try {
        std::lock_guard lock(state_->slots_mutex);
        if (!state_->configured) return;
        state_->configured = false;
        for (const auto& slot : state_->slots) {
            if (slot && slot->resource) {
                slot->resource->retire_producer_access();
            }
        }
    } catch (...) {
        // Retirement runs during device/profile teardown and must not prevent
        // the Host from completing shutdown. Platform resources still retain
        // their ordinary destructor fallback.
    }
}

StageFrameWriteLease StageFramePool::try_acquire() noexcept {
    if (!state_) return {};
    std::unique_lock lock(state_->slots_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !state_->configured) return {};
    const auto slot_count = state_->slots.size();
    for (std::size_t offset = 0; offset < slot_count; ++offset) {
        const auto index = (state_->next_slot + offset) % slot_count;
        const auto& slot = state_->slots[index];
        if (slot->active_read_lease.load(std::memory_order_acquire) != 0) {
            continue;
        }
        bool expected = false;
        if (!slot->write_claimed.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            continue;
        }
        try {
            auto impl = std::make_unique<StageFrameWriteLease::Impl>();
            impl->pool = state_;
            impl->slot = slot;
            // Flutter's DXGI release callback fires once the shared handle has
            // been opened, not once the raster GPU has finished sampling it.
            // Rotate otherwise-free slots so the producer does not rewrite
            // the same texture on consecutive frames while Flutter still has
            // that resource in flight.
            state_->next_slot = (index + 1) % slot_count;
            return StageFrameWriteLease(std::move(impl));
        } catch (...) {
            slot->write_claimed.store(false, std::memory_order_release);
            return {};
        }
    }
    return {};
}

StageFrameMailbox::StageFrameMailbox(
        std::shared_ptr<detail::StageFrameMailboxState> state)
    : state_(std::move(state)) {}

StageFrameMailbox::~StageFrameMailbox() = default;
StageFrameMailbox::StageFrameMailbox(StageFrameMailbox&&) noexcept = default;
StageFrameMailbox& StageFrameMailbox::operator=(StageFrameMailbox&&) noexcept = default;

StageFrameLease StageFrameMailbox::take_latest() noexcept {
    if (!state_) return {};
    return StageFrameLease(
        state_->latest.exchange(nullptr, std::memory_order_acq_rel));
}

StageFrameLease StageFrameMailbox::peek_latest() const noexcept {
    if (!state_) return {};
    return StageFrameLease(state_->latest.load(std::memory_order_acquire));
}

std::uint64_t StageFrameMailbox::dropped_frames() const noexcept {
    return state_
        ? state_->dropped_frames.load(std::memory_order_relaxed)
        : 0;
}

struct StageFrameHub::Impl {
    mutable std::mutex subscribers_mutex;
    std::mutex publish_mutex;
    mutable std::vector<
        std::weak_ptr<detail::StageFrameMailboxState>> subscribers;

    [[nodiscard]] std::vector<
            std::shared_ptr<detail::StageFrameMailboxState>>
            live_subscribers() const {
        std::vector<std::shared_ptr<detail::StageFrameMailboxState>> result;
        std::lock_guard lock(subscribers_mutex);
        auto output = subscribers.begin();
        for (auto input = subscribers.begin(); input != subscribers.end(); ++input) {
            if (auto subscriber = input->lock()) {
                result.push_back(std::move(subscriber));
                *output++ = *input;
            }
        }
        subscribers.erase(output, subscribers.end());
        return result;
    }
};

StageFrameHub::StageFrameHub() : impl_(std::make_unique<Impl>()) {}
StageFrameHub::~StageFrameHub() = default;
StageFrameHub::StageFrameHub(StageFrameHub&&) noexcept = default;
StageFrameHub& StageFrameHub::operator=(StageFrameHub&&) noexcept = default;

StageFrameMailbox StageFrameHub::subscribe() {
    if (!impl_) return {};
    auto state = std::make_shared<detail::StageFrameMailboxState>();
    {
        std::lock_guard lock(impl_->subscribers_mutex);
        impl_->subscribers.push_back(state);
    }
    return StageFrameMailbox(std::move(state));
}

std::size_t StageFrameHub::publish(const StageFrameLease& frame) noexcept {
    if (!impl_ || !frame) return 0;
    try {
        std::lock_guard publish_lock(impl_->publish_mutex);
        auto subscribers = impl_->live_subscribers();
        return static_cast<std::size_t>(std::ranges::count_if(
            subscribers, [&frame](const auto& subscriber) {
                return subscriber->offer(frame.state_);
            }));
    } catch (...) {
        return 0;
    }
}

std::size_t StageFrameHub::subscriber_count() const noexcept {
    if (!impl_) return 0;
    try {
        return impl_->live_subscribers().size();
    } catch (...) {
        return 0;
    }
}

} // namespace kirakara::show
