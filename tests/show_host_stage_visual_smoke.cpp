#include "../apps/show_host/show_host_api.h"

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void pump_for(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    MSG message{};
    while (std::chrono::steady_clock::now() < deadline) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        std::this_thread::sleep_for(2ms);
    }
}

template <typename Predicate>
bool pump_until(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        pump_for(10ms);
    }
    return predicate();
}

ID3D11Device* create_stage_device() {
    constexpr D3D_FEATURE_LEVEL levels[]{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    ID3D11Device* device{};
    HRESULT result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT
            | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &device,
        nullptr,
        nullptr);
    if (result == E_INVALIDARG) {
        result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT
                | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            levels + 1,
            1,
            D3D11_SDK_VERSION,
            &device,
            nullptr,
            nullptr);
    }
    return SUCCEEDED(result) ? device : nullptr;
}

class StageVisualConsumer final {
public:
    explicit StageVisualConsumer(ID3D11Device* device) : device_(device) {
        expect(device_ != nullptr, "consumer device is null");
        device_->AddRef();
        expect(SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&device1_))),
            "consumer device has no ID3D11Device1");
        device_->GetImmediateContext(&context_);
        expect(context_ != nullptr, "consumer device has no immediate context");
        worker_ = std::thread(&StageVisualConsumer::run, this);
    }

    ~StageVisualConsumer() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
            release_hold_ = true;
        }
        condition_.notify_all();
        if (worker_.joinable()) worker_.join();
        close_pending_handle();
        if (copy_texture_) copy_texture_->Release();
        if (keyed_mutex_) keyed_mutex_->Release();
        if (shared_texture_) shared_texture_->Release();
        context_->Release();
        device1_->Release();
        device_->Release();
    }

    StageVisualConsumer(const StageVisualConsumer&) = delete;
    StageVisualConsumer& operator=(const StageVisualConsumer&) = delete;

    static void callback(
            void* user_data, const ShowHostStageVisualFrame* frame) {
        auto* self = static_cast<StageVisualConsumer*>(user_data);
        if (!self || !frame) return;
        self->enqueue(*frame);
    }

    void hold_next_frame() {
        std::lock_guard lock(mutex_);
        hold_next_ = true;
        release_hold_ = false;
    }

    bool wait_until_holding(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [&] {
            return holding_ || stop_;
        }) && holding_;
    }

    void release_held_frame() {
        {
            std::lock_guard lock(mutex_);
            release_hold_ = true;
        }
        condition_.notify_all();
    }

    std::uint64_t callbacks() const noexcept {
        return callbacks_.load(std::memory_order_relaxed);
    }
    std::uint64_t acquired_frames() const noexcept {
        return acquired_frames_.load(std::memory_order_relaxed);
    }
    std::uint64_t resource_generation() const noexcept {
        return resource_generation_.load(std::memory_order_relaxed);
    }
    std::uint64_t last_frame_id() const noexcept {
        return last_frame_id_.load(std::memory_order_relaxed);
    }
    std::uint64_t invalid_descriptors() const noexcept {
        return invalid_descriptors_.load(std::memory_order_relaxed);
    }
    std::uint64_t acquire_timeouts() const noexcept {
        return acquire_timeouts_.load(std::memory_order_relaxed);
    }

private:
    void enqueue(const ShowHostStageVisualFrame& frame) {
        if (frame.struct_size != sizeof(frame)
                || frame.abi_version != SHOW_HOST_STAGE_VISUAL_ABI_VERSION
                || frame.sync_type != SHOW_HOST_STAGE_VISUAL_SYNC_KEYED_MUTEX
                || frame.shared_nt_handle == 0
                || frame.width == 0 || frame.height == 0
                || frame.resource_generation == 0
                || frame.consumer_acquire_key == frame.consumer_release_key) {
            invalid_descriptors_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        HANDLE duplicate{};
        if (!DuplicateHandle(
                GetCurrentProcess(),
                reinterpret_cast<HANDLE>(frame.shared_nt_handle),
                GetCurrentProcess(),
                &duplicate,
                0,
                FALSE,
                DUPLICATE_SAME_ACCESS)) {
            invalid_descriptors_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        {
            std::lock_guard lock(mutex_);
            if (pending_handle_) CloseHandle(pending_handle_);
            pending_ = frame;
            pending_.shared_nt_handle = reinterpret_cast<std::uintptr_t>(
                duplicate);
            pending_handle_ = duplicate;
            pending_ready_ = true;
        }
        callbacks_.fetch_add(1, std::memory_order_relaxed);
        condition_.notify_one();
    }

    void close_pending_handle() noexcept {
        if (pending_handle_) {
            CloseHandle(pending_handle_);
            pending_handle_ = nullptr;
        }
    }

    bool open_resource(const ShowHostStageVisualFrame& frame, HANDLE handle) {
        ID3D11Texture2D* texture{};
        const HRESULT open_result = device1_->OpenSharedResource1(
            handle, IID_PPV_ARGS(&texture));
        if (FAILED(open_result) || !texture) return false;

        IDXGIKeyedMutex* keyed_mutex{};
        if (FAILED(texture->QueryInterface(IID_PPV_ARGS(&keyed_mutex)))
                || !keyed_mutex) {
            texture->Release();
            return false;
        }

        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (description.Width != frame.width
                || description.Height != frame.height
                || static_cast<std::uint32_t>(description.Format)
                    != frame.dxgi_format) {
            keyed_mutex->Release();
            texture->Release();
            return false;
        }
        description.MiscFlags = 0;
        ID3D11Texture2D* copy{};
        if (FAILED(device_->CreateTexture2D(&description, nullptr, &copy))) {
            keyed_mutex->Release();
            texture->Release();
            return false;
        }

        if (copy_texture_) copy_texture_->Release();
        if (keyed_mutex_) keyed_mutex_->Release();
        if (shared_texture_) shared_texture_->Release();
        copy_texture_ = copy;
        keyed_mutex_ = keyed_mutex;
        shared_texture_ = texture;
        opened_resource_generation_ = frame.resource_generation;
        resource_generation_.store(
            frame.resource_generation, std::memory_order_relaxed);
        return true;
    }

    void consume(ShowHostStageVisualFrame frame, HANDLE handle) {
        if (frame.resource_generation != opened_resource_generation_
                && !open_resource(frame, handle)) {
            invalid_descriptors_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!shared_texture_ || !keyed_mutex_ || !copy_texture_) return;

        HRESULT acquire = keyed_mutex_->AcquireSync(
            frame.consumer_acquire_key, 0);
        if (acquire == WAIT_TIMEOUT) {
            acquire_timeouts_.fetch_add(1, std::memory_order_relaxed);
            // Show invokes the callback only after ReleaseSync succeeds. A
            // zero-timeout miss therefore means that cross-device GPU work is
            // still completing, not that this descriptor is safe to discard.
            // Dropping it would strand the mutex at the consumer key and make
            // all later non-blocking producer acquires fail. This dedicated
            // consumer thread waits exactly once and never backpressures Show.
            acquire = keyed_mutex_->AcquireSync(
                frame.consumer_acquire_key, INFINITE);
        }
        if (FAILED(acquire)) {
            invalid_descriptors_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        context_->CopyResource(copy_texture_, shared_texture_);
        {
            std::unique_lock lock(mutex_);
            if (hold_next_) {
                hold_next_ = false;
                holding_ = true;
                condition_.notify_all();
                condition_.wait(lock, [&] { return release_hold_ || stop_; });
                holding_ = false;
            }
        }
        const HRESULT release = keyed_mutex_->ReleaseSync(
            frame.consumer_release_key);
        if (FAILED(release)) {
            invalid_descriptors_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        acquired_frames_.fetch_add(1, std::memory_order_relaxed);
        last_frame_id_.store(frame.frame_id, std::memory_order_relaxed);
    }

    void run() {
        while (true) {
            ShowHostStageVisualFrame frame{};
            HANDLE handle{};
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [&] { return stop_ || pending_ready_; });
                if (stop_) break;
                frame = pending_;
                handle = pending_handle_;
                pending_handle_ = nullptr;
                pending_ready_ = false;
            }
            consume(frame, handle);
            if (handle) CloseHandle(handle);
        }
    }

    ID3D11Device* device_{};
    ID3D11Device1* device1_{};
    ID3D11DeviceContext* context_{};
    ID3D11Texture2D* shared_texture_{};
    ID3D11Texture2D* copy_texture_{};
    IDXGIKeyedMutex* keyed_mutex_{};
    std::uint64_t opened_resource_generation_{};

    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    ShowHostStageVisualFrame pending_{};
    HANDLE pending_handle_{};
    bool pending_ready_{};
    bool stop_{};
    bool hold_next_{};
    bool holding_{};
    bool release_hold_{};

    std::atomic_uint64_t callbacks_{};
    std::atomic_uint64_t acquired_frames_{};
    std::atomic_uint64_t resource_generation_{};
    std::atomic_uint64_t last_frame_id_{};
    std::atomic_uint64_t invalid_descriptors_{};
    std::atomic_uint64_t acquire_timeouts_{};
};

ShowHostStageVisualStats read_stats(
        const ShowHostStageVisualApi& api,
        ShowHostStageVisualSource source) {
    ShowHostStageVisualStats stats{};
    stats.struct_size = sizeof(stats);
    stats.abi_version = SHOW_HOST_STAGE_VISUAL_ABI_VERSION;
    expect(api.get_stats(source, &stats) == SHOW_HOST_STAGE_VISUAL_SUCCESS,
        "read Stage Visual statistics");
    return stats;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    expect(argc == 5,
        "usage: stage-visual-smoke video krl vocal accompaniment");

    ShowHostStageVisualApi api{};
    api.struct_size = sizeof(api);
    expect(show_host_get_stage_visual_api(
            SHOW_HOST_STAGE_VISUAL_ABI_VERSION, &api)
            == SHOW_HOST_STAGE_VISUAL_SUCCESS,
        "resolve Stage Visual API");

    ID3D11Device* producer_device = create_stage_device();
    ID3D11Device* consumer_device = create_stage_device();
    expect(producer_device && consumer_device,
        "create independent producer and consumer D3D11 devices");
    StageVisualConsumer consumer(consumer_device);

    ShowHostHandle host = show_host_create();
    expect(host != nullptr, "create ShowHost");
    expect(show_host_set_stage_d3d_device(
            host, reinterpret_cast<std::uintptr_t>(producer_device)),
        "select producer D3D11 device");

    ShowHostStageVisualSource source{};
    expect(api.create_source(host, &source) == SHOW_HOST_STAGE_VISUAL_SUCCESS
            && source,
        "create Stage Visual source");
    api.set_frame_callback(source, &StageVisualConsumer::callback, &consumer);
    api.set_active(source, true);

    expect(show_host_load_with_options(
            host, argv[1], argv[2], argv[3], argv[4],
            SHOW_CLOCK_AUDIO_MASTER, SHOW_TRANSITION_HARD),
        "load Stage Visual smoke song");
    show_host_play(host);
    expect(pump_until([&] {
            return consumer.acquired_frames() >= 30
                && show_host_get_state(host) == SHOW_STATE_PLAYING
                && show_host_get_position(host) > 0.5;
        }, 6s),
        "consume thirty synchronized Stage Visual frames while playback advances");
    expect(show_host_get_state(host) == SHOW_STATE_PLAYING
            && show_host_get_position(host) > 0.5,
        "playback clock advances with composed preview active");
    expect(consumer.invalid_descriptors() == 0
            && consumer.acquire_timeouts() == 0
            && consumer.resource_generation() != 0
            && consumer.last_frame_id() != 0,
        "Stage Visual descriptors and keyed-mutex handoff are valid");

    const auto before_hold = read_stats(api, source);
    ShowHostStageFrameStats stage_before_hold{};
    stage_before_hold.struct_size = sizeof(stage_before_hold);
    expect(show_host_get_stage_frame_stats(host, &stage_before_hold),
        "read Stage counters before consumer stall");
    const double position_before_hold = show_host_get_position(host);
    consumer.hold_next_frame();
    expect(consumer.wait_until_holding(2s),
        "consumer acquires the frame selected for a stall");
    pump_for(400ms);
    const auto during_hold = read_stats(api, source);
    ShowHostStageFrameStats stage_during_hold{};
    stage_during_hold.struct_size = sizeof(stage_during_hold);
    expect(show_host_get_stage_frame_stats(host, &stage_during_hold),
        "read Stage counters during consumer stall");
    const double position_during_hold = show_host_get_position(host);
    expect(position_during_hold > position_before_hold + 0.15,
        "consumer stall must not block the Show playback clock");
    expect(stage_during_hold.produced_frames
            > stage_before_hold.produced_frames + 10,
        "consumer stall must not backpressure Stage frame production");
    expect(during_hold.producer_busy_drops
            > before_hold.producer_busy_drops,
        "busy consumer drops preview frames without waiting");
    consumer.release_held_frame();
    const auto acquired_before_recovery = consumer.acquired_frames();
    expect(pump_until([&] {
            return consumer.acquired_frames() > acquired_before_recovery;
        }, 2s),
        "Stage Visual delivery recovers after consumer stall");

    const auto generation_before_device_change =
        consumer.resource_generation();
    ID3D11Device* replacement_device = create_stage_device();
    expect(replacement_device != nullptr, "create replacement producer device");
    expect(show_host_set_stage_d3d_device(
            host, reinterpret_cast<std::uintptr_t>(replacement_device)),
        "replace Stage producer D3D11 device");
    expect(pump_until([&] {
            return consumer.resource_generation()
                > generation_before_device_change;
        }, 4s),
        "resource generation advances after device replacement");

    const auto callbacks_before_detach = consumer.callbacks();
    api.set_active(source, false);
    pump_for(300ms);
    expect(consumer.callbacks() <= callbacks_before_detach + 1,
        "detached Stage Visual source stops callbacks");
    const auto generation_before_reattach = consumer.resource_generation();
    api.set_active(source, true);
    expect(pump_until([&] {
            return consumer.resource_generation()
                > generation_before_reattach;
        }, 3s),
        "reattached Stage Visual source creates a fresh resource generation");

    const auto final_stats = read_stats(api, source);
    expect(final_stats.published_frames >= consumer.acquired_frames()
            && final_stats.callback_count == consumer.callbacks()
            && final_stats.resource_recreations >= 3
            && final_stats.last_hresult == S_OK,
        "Stage Visual final counters are internally consistent");

    api.set_active(source, false);
    api.set_frame_callback(source, nullptr, nullptr);
    api.destroy_source(source);
    show_host_destroy(host);
    replacement_device->Release();
    producer_device->Release();
    consumer_device->Release();

    std::cout << "Stage Visual NT-handle/keyed-mutex smoke passed: callbacks="
              << consumer.callbacks()
              << " acquired=" << consumer.acquired_frames()
              << " busy_drops=" << final_stats.producer_busy_drops
              << " resource_generation=" << final_stats.resource_generation
              << '\n';
    return EXIT_SUCCESS;
}
