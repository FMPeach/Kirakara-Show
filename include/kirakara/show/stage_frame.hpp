#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace kirakara::show {

enum class StageOutputRole {
    controller_preview,
    physical_display,
    cast,
};

enum class StagePixelFormat {
    bgra8,
};

struct StageOutputProfile {
    StageOutputRole role{StageOutputRole::controller_preview};
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    std::uint32_t refresh_rate_num{60};
    std::uint32_t refresh_rate_den{1};
    StagePixelFormat pixel_format{StagePixelFormat::bgra8};

    [[nodiscard]] bool valid() const noexcept;
};

struct StageFrameTiming {
    std::uint64_t generation{};
    std::uint64_t frame_id{};
    std::int64_t pts_100ns{};
    std::int64_t duration_100ns{};
};

struct StageFrame {
    StageOutputProfile profile;
    StageFrameTiming timing;
    void* native_resource{};
    std::uintptr_t shared_handle{};
};

// Platform implementations own the actual GPU texture and optional shared
// handle. The pool controls when the resource may be written again.
class StageFrameResource {
public:
    virtual ~StageFrameResource() = default;
    [[nodiscard]] virtual void* native_resource() noexcept = 0;
    [[nodiscard]] virtual std::uintptr_t shared_handle() const noexcept = 0;

    // Called by the producer before a frame pool generation is retired.
    // Implementations can release thread-affine writer state while retaining
    // the immutable resource that outstanding Sink leases still consume.
    virtual void retire_producer_access() noexcept {}
};

using StageFrameResourceFactory = std::function<
    std::shared_ptr<StageFrameResource>(
        const StageOutputProfile&, std::size_t slot_index)>;

namespace detail {
struct StageFrameLeaseState;
struct StageFramePoolState;
struct StageFrameMailboxState;
} // namespace detail

class StageFrameLease {
public:
    StageFrameLease() = default;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] const StageFrame* get() const noexcept;
    [[nodiscard]] const StageFrame* operator->() const noexcept;

private:
    explicit StageFrameLease(
        std::shared_ptr<const detail::StageFrameLeaseState> state);

    std::shared_ptr<const detail::StageFrameLeaseState> state_;

    friend class StageFrameWriteLease;
    friend class StageFrameMailbox;
    friend class StageFrameHub;
};

class StageFrameWriteLease {
public:
    StageFrameWriteLease() = default;
    ~StageFrameWriteLease();

    StageFrameWriteLease(const StageFrameWriteLease&) = delete;
    StageFrameWriteLease& operator=(const StageFrameWriteLease&) = delete;
    StageFrameWriteLease(StageFrameWriteLease&&) noexcept;
    StageFrameWriteLease& operator=(StageFrameWriteLease&&) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept;
    // Writable resources are visible only to the producer while the slot is
    // exclusively claimed. Consumers continue to see immutable frame data.
    [[nodiscard]] StageFrameResource* resource() noexcept;
    [[nodiscard]] void* native_resource() noexcept;
    [[nodiscard]] std::uintptr_t shared_handle() const noexcept;
    [[nodiscard]] StageOutputProfile profile() const noexcept;

    // Publishes immutable frame metadata and transfers this writable slot into
    // a read lease. An invalid timing value cancels the write and returns empty.
    [[nodiscard]] StageFrameLease publish(StageFrameTiming timing);

private:
    struct Impl;
    explicit StageFrameWriteLease(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class StageFramePool;
};

class StageFramePool {
public:
    static constexpr std::size_t default_capacity = 3;

    StageFramePool();
    ~StageFramePool();

    StageFramePool(const StageFramePool&) = delete;
    StageFramePool& operator=(const StageFramePool&) = delete;
    StageFramePool(StageFramePool&&) noexcept;
    StageFramePool& operator=(StageFramePool&&) noexcept;

    [[nodiscard]] bool configure(
        StageOutputProfile profile,
        StageFrameResourceFactory resource_factory,
        std::size_t capacity = default_capacity);
    [[nodiscard]] bool configured() const noexcept;
    [[nodiscard]] StageOutputProfile profile() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t available() const noexcept;

    // Stops new writes and retires producer-only state for every slot. Read
    // leases remain valid until their consumers release them.
    void retire_producer_access() noexcept;

    // Never waits for a Sink. Returns empty when every slot is leased or when
    // another thread is reconfiguring the pool.
    [[nodiscard]] StageFrameWriteLease try_acquire() noexcept;

private:
    std::shared_ptr<detail::StageFramePoolState> state_;
};

class StageFrameMailbox {
public:
    StageFrameMailbox() = default;
    ~StageFrameMailbox();

    StageFrameMailbox(const StageFrameMailbox&) = delete;
    StageFrameMailbox& operator=(const StageFrameMailbox&) = delete;
    StageFrameMailbox(StageFrameMailbox&&) noexcept;
    StageFrameMailbox& operator=(StageFrameMailbox&&) noexcept;

    // take_latest consumes the current mailbox entry. peek_latest keeps it in
    // place. Both return a lease, so the backing pool slot remains immutable.
    [[nodiscard]] StageFrameLease take_latest() noexcept;
    [[nodiscard]] StageFrameLease peek_latest() const noexcept;
    [[nodiscard]] std::uint64_t dropped_frames() const noexcept;

private:
    explicit StageFrameMailbox(
        std::shared_ptr<detail::StageFrameMailboxState> state);
    std::shared_ptr<detail::StageFrameMailboxState> state_;

    friend class StageFrameHub;
};

class StageFrameHub {
public:
    StageFrameHub();
    ~StageFrameHub();

    StageFrameHub(const StageFrameHub&) = delete;
    StageFrameHub& operator=(const StageFrameHub&) = delete;
    StageFrameHub(StageFrameHub&&) noexcept;
    StageFrameHub& operator=(StageFrameHub&&) noexcept;

    [[nodiscard]] StageFrameMailbox subscribe();

    // Offers the frame to every live mailbox. Each mailbox keeps only its
    // newest frame; no Sink can block another Sink or the producer.
    [[nodiscard]] std::size_t publish(const StageFrameLease& frame) noexcept;
    [[nodiscard]] std::size_t subscriber_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kirakara::show
