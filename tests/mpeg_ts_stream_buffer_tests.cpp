#include "../apps/show_host/cast/mpeg_ts_stream_buffer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << "\n";
        std::exit(1);
    }
}

void reads_appended_bytes_in_order() {
    MpegTsStreamBuffer buffer(64);
    auto cursor = buffer.cursor_from_head();
    const std::vector<std::uint8_t> first{1, 2, 3};
    const std::vector<std::uint8_t> second{4, 5};
    static_cast<void>(buffer.append(first.data(), first.size()));
    static_cast<void>(buffer.append(second.data(), second.size()));

    std::uint8_t out[8]{};
    const auto result = buffer.read(
        cursor, out, sizeof(out), std::chrono::milliseconds(1));

    expect(result.bytes == 5, "unexpected byte count");
    expect(!result.reset, "fresh reader should not reset");
    expect(!result.timed_out, "read should not time out");
    expect(std::vector<std::uint8_t>(out, out + result.bytes)
            == std::vector<std::uint8_t>({1, 2, 3, 4, 5}),
        "payload order mismatch");
}

void takes_ownership_of_complete_chunks_without_changing_order() {
    MpegTsStreamBuffer buffer(64);
    auto cursor = buffer.cursor_from_head();
    std::vector<std::uint8_t> first{1, 2, 3, 4};
    static_cast<void>(buffer.append(std::move(first)));
    const std::vector<std::uint8_t> second{5, 6};
    static_cast<void>(buffer.append(second.data(), second.size()));

    std::uint8_t out[8]{};
    const auto result = buffer.read(
        cursor, out, sizeof(out), std::chrono::milliseconds(1));

    expect(result.bytes == 6, "owned chunks returned wrong byte count");
    expect(!result.reset, "owned chunks unexpectedly reset cursor");
    expect(std::vector<std::uint8_t>(out, out + result.bytes)
            == std::vector<std::uint8_t>({1, 2, 3, 4, 5, 6}),
        "owned chunk order mismatch");
}

void waits_for_new_data() {
    MpegTsStreamBuffer buffer(64);
    auto cursor = buffer.cursor_from_tail();
    std::uint8_t out[4]{};
    std::atomic<std::size_t> bytes{};

    std::thread reader([&] {
        const auto result = buffer.read(
            cursor, out, sizeof(out), std::chrono::milliseconds(500));
        expect(!result.timed_out, "reader timed out before append");
        bytes = result.bytes;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const std::vector<std::uint8_t> payload{9, 8, 7};
    static_cast<void>(buffer.append(payload.data(), payload.size()));
    reader.join();

    expect(bytes == 3, "waiting reader received wrong byte count");
    expect(std::vector<std::uint8_t>(out, out + bytes.load()) == payload,
        "waiting reader received wrong payload");
}

void stale_readers_are_reset_to_buffer_head() {
    MpegTsStreamBuffer buffer(6);
    auto cursor = buffer.cursor_from_head();
    const std::vector<std::uint8_t> payload{1, 2, 3, 4, 5, 6, 7, 8};
    static_cast<void>(buffer.append(payload.data(), payload.size()));

    std::uint8_t out[8]{};
    const auto result = buffer.read(
        cursor, out, sizeof(out), std::chrono::milliseconds(1));

    expect(result.reset, "stale reader should be reset");
    expect(result.bytes == 6, "trimmed read should return retained tail");
    expect(std::vector<std::uint8_t>(out, out + result.bytes)
            == std::vector<std::uint8_t>({3, 4, 5, 6, 7, 8}),
        "trimmed payload mismatch");
}

void reset_wakes_reader_and_advances_generation() {
    MpegTsStreamBuffer buffer(64);
    auto cursor = buffer.cursor_from_tail();
    std::uint8_t out[4]{};
    std::atomic<bool> reset{};

    std::thread reader([&] {
        const auto result = buffer.read(
            cursor, out, sizeof(out), std::chrono::milliseconds(500));
        reset = result.reset;
        expect(!result.timed_out, "reset should wake reader");
        expect(result.bytes == 0, "reset without data should not fake bytes");
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    buffer.reset();
    reader.join();

    expect(reset, "reader did not observe reset");
}

}  // namespace

int main() {
    reads_appended_bytes_in_order();
    takes_ownership_of_complete_chunks_without_changing_order();
    waits_for_new_data();
    stale_readers_are_reset_to_buffer_head();
    reset_wakes_reader_and_advances_generation();
    std::cout << "MpegTsStreamBuffer test passed.\n";
    return 0;
}
