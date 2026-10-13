#include "../apps/show_host/show_host_api.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
template<class Predicate>
void wait_for(Predicate condition, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(false, message);
}
}
int wmain(int argc, wchar_t** argv) {
    expect(argc == 2, "pass the codec fixture directory");
    const std::filesystem::path directory(argv[1]);
    auto host = show_host_create();
    expect(host != nullptr, "create host");
    for (const auto* video : {L"视频与音频.webm", L"hevc-opus.mkv", L"av1-10.mkv", L"hevc-10.mp4",
            L"h264-8.mp4", L"h264-aac.mp4"}) {
        for (const auto clock : {SHOW_CLOCK_AUDIO_MASTER, SHOW_CLOCK_VIDEO_MASTER}) {
            std::cout << "clock " << clock << ", " << std::filesystem::path(video).extension().string() << std::endl;
            const auto source = (directory / video).wstring();
            expect(show_host_load_with_clock(host, source.c_str(), L"", L"", L"", clock),
                "load AV1/HEVC without Media Engine codec extensions");
            show_host_set_volume(host, 0); // Process audio without audible test tones.
            // Cover the paced PCM worker with the existing pitch-shift path.
            if (std::wstring_view(video) == L"h264-aac.mp4") show_host_set_key_semitones(host, 3);
            else show_host_set_key_semitones(host, 0);
            wait_for([&] { return std::abs(show_host_get_duration(host) - 2.0) < 0.1; }, "host media duration");
            show_host_play(host);
            wait_for([&] { return show_host_get_position(host) >= 0.2; }, "host playback clock advances");
            expect(!show_host_is_buffering(host), "new format is not waiting for Media Engine");
            show_host_pause(host);
            wait_for([&] { return show_host_get_state(host) == SHOW_STATE_PAUSED; }, "pause new format");
            // State is published before the posted handler has stopped the
            // device. A synchronous command fences completion of that handler.
            show_host_set_volume(host, 0);
            const auto paused = show_host_get_position(host);
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            const auto after_pause = show_host_get_position(host);
            if (std::abs(after_pause - paused) >= 0.04)
                std::cerr << "paused position " << paused << " -> " << after_pause << '\n';
            expect(std::abs(after_pause - paused) < 0.04, "pause freezes media clock");
            show_host_seek(host, 1.5);
            wait_for([&] { return std::abs(show_host_get_position(host) - 1.5) < 0.04; }, "seek paused format");
            show_host_play(host);
            wait_for([&] { return show_host_get_state(host) == SHOW_STATE_STOPPED; }, "new format reaches natural end");
        }
    }
    show_host_destroy(host);
    std::cout << "AV1/HEVC and embedded Opus playback tests passed\n";
}
