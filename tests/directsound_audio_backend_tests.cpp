#include "../apps/show_host/audio/directsound_backend.h"

#include <mfapi.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint64_t ticks(const FILETIME& time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32)
        | time.dwLowDateTime;
}

double cpu_seconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    expect(GetProcessTimes(GetCurrentProcess(), &created, &exited,
        &kernel, &user) != FALSE, "cannot sample process CPU time");
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10'000'000.0;
}

double measure_cpu(const char* label, std::chrono::milliseconds duration) {
    const auto started = std::chrono::steady_clock::now();
    const auto before = cpu_seconds();
    std::this_thread::sleep_for(duration);
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    const auto one_core = (cpu_seconds() - before) / elapsed;
    std::cout << label << " one_core_cpu=" << std::fixed
        << std::setprecision(2) << one_core * 100.0 << "%" << std::endl;
    return one_core;
}

void write_u16(std::ostream& output, std::uint16_t value) {
    output.put(static_cast<char>(value & 0xff));
    output.put(static_cast<char>((value >> 8) & 0xff));
}

void write_u32(std::ostream& output, std::uint32_t value) {
    write_u16(output, static_cast<std::uint16_t>(value));
    write_u16(output, static_cast<std::uint16_t>(value >> 16));
}

// Self-contained fixtures: no downloaded media or FFmpeg dependency.
struct Fixtures {
    std::filesystem::path directory;
    std::vector<std::filesystem::path> files;

    Fixtures() {
        directory = std::filesystem::temp_directory_path()
            / ("kirakara-pcm-tests-" + std::to_string(GetCurrentProcessId()));
        expect(std::filesystem::create_directory(directory),
            "fixture directory already exists");
    }

    ~Fixtures() {
        std::error_code ignored;
        for (const auto& file : files) std::filesystem::remove(file, ignored);
        std::filesystem::remove(directory, ignored);
    }

    std::wstring make_wave(int rate, int frequency) {
        const auto file = directory / (std::to_string(rate) + "-"
            + std::to_string(frequency) + ".wav");
        files.push_back(file);
        std::ofstream output(file, std::ios::binary);
        constexpr int seconds = 8;
        const auto frames = rate * seconds;
        const auto bytes = static_cast<std::uint32_t>(frames * 4);
        output.write("RIFF", 4); write_u32(output, bytes + 36);
        output.write("WAVEfmt ", 8); write_u32(output, 16);
        write_u16(output, 1); write_u16(output, 2);
        write_u32(output, rate); write_u32(output, rate * 4);
        write_u16(output, 4); write_u16(output, 16);
        output.write("data", 4); write_u32(output, bytes);
        for (int frame = 0; frame < frames; ++frame) {
            const auto sample = static_cast<std::int16_t>(
                std::sin(frame * 6.283185307179586 * frequency / rate) * 8000);
            write_u16(output, static_cast<std::uint16_t>(sample));
            write_u16(output, static_cast<std::uint16_t>(sample));
        }
        expect(output.good(), "cannot write PCM fixture");
        return file.wstring();
    }
};

void exercise_backend(Fixtures& fixtures, int rate, bool benchmark_only) {
    const auto vocal = fixtures.make_wave(rate, 440);
    const auto accompaniment = fixtures.make_wave(rate, 220);
    DirectSoundPcmAudioBackend audio;
    audio.set_local_output_enabled(false); // Never play test tones to speakers.
    expect(audio.open(vocal), "cannot open PCM fixture");
    expect(audio.load_second_track(accompaniment), "cannot open second track");
    expect(audio.processed_pcm_format().sample_rate == rate,
        "sample rate changed unexpectedly");
    std::cout << "sample_rate=" << rate << std::endl;
    measure_cpu("standby", 250ms);
    expect(audio.play(), "play failed");
    const auto plain_cpu = measure_cpu("playing/key=0", 1200ms);
    const auto position = audio.position();
    expect(position > 0.8 && position < 1.8, "audio clock did not advance");
    std::vector<std::int16_t> pcm(512 * 2);
    expect(audio.read_processed_pcm_at(
        static_cast<std::uint64_t>(position * rate), pcm.data(), 512) == 512,
        "Cast timeline PCM is not available at the playback position");
    expect(std::any_of(pcm.begin(), pcm.end(), [](auto sample) {
        return sample != 0;
    }), "processed audio unexpectedly contains only silence");

    expect(audio.pause(), "pause failed");
    const auto paused = audio.position();
    measure_cpu("paused", 250ms);
    expect(std::abs(audio.position() - paused) < 0.02,
        "paused audio clock advanced");
    expect(audio.play(), "resume failed");
    std::this_thread::sleep_for(150ms);
    expect(audio.position() > paused + 0.05, "resume did not advance audio");

    expect(audio.set_key_semitones(3), "pitch shift failed");
    measure_cpu("playing/key=3", 500ms);
    const auto before_switch = audio.position();
    audio.set_active_track(SHOW_AUDIO_TRACK_ACCOMPANIMENT);
    std::this_thread::sleep_for(150ms);
    expect(audio.position() > before_switch,
        "track switch did not preserve the running audio clock");
    audio.set_volume(38);
    expect(audio.set_key_semitones(-3), "negative pitch shift failed");
    std::this_thread::sleep_for(100ms);

    expect(audio.seek(0.0), "replay seek failed");
    expect(audio.play(), "replay failed");
    std::this_thread::sleep_for(150ms);
    expect(audio.position() > 0.03 && audio.position() < 0.6,
        "replay did not restart the audio clock near zero");
    expect(audio.stop(), "stop failed");
    expect(audio.position() < 0.02, "stop did not reset the audio clock");
    for (int iteration = 0; iteration < 5; ++iteration) {
        expect(audio.play(), "rapid play failed");
        expect(audio.pause(), "rapid pause failed");
        expect(audio.seek(0.0), "rapid seek failed");
    }
    expect(audio.set_key_semitones(0), "pitch reset failed");
    expect(audio.seek(audio.duration() - 0.2), "EOF seek failed");
    expect(audio.play(), "EOF play failed");
    const auto deadline = std::chrono::steady_clock::now() + 1500ms;
    while (!audio.has_ended() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    expect(audio.has_ended(), "audio EOF was not reported");
    const auto close_started = std::chrono::steady_clock::now();
    audio.close();
    expect(std::chrono::steady_clock::now() - close_started < 1s,
        "closing an idle audio worker was blocked");
    expect(audio.open(vocal), "cannot reopen the preloaded audio player");
    std::this_thread::sleep_for(20ms);
    const auto standby_close_started = std::chrono::steady_clock::now();
    audio.close();
    expect(std::chrono::steady_clock::now() - standby_close_started < 1s,
        "closing a preloaded audio worker was blocked");
    // A deliberately generous one-core budget catches a tight busy loop,
    // not small differences between machines or the cost of pitch shifting.
    if (!benchmark_only) {
        expect(plain_cpu < 0.15, "unshifted PCM worker is busy-spinning");
    }
}

} // namespace

int main(int argc, char** argv) try {
    IDirectSound8* device{};
    const auto device_result = DirectSoundCreate8(nullptr, &device, nullptr);
    if (FAILED(device_result)) {
        std::cout << "SKIP: DirectSound playback device is unavailable" << std::endl;
        return 77;
    }
    device->Release();
    expect(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)),
        "COM initialization failed");
    expect(SUCCEEDED(MFStartup(MF_VERSION)), "Media Foundation startup failed");
    int result = EXIT_SUCCESS;
    try {
        Fixtures fixtures;
        const bool benchmark_only = argc > 1
            && std::string_view(argv[1]) == "--benchmark-only";
        exercise_backend(fixtures, 44100, benchmark_only);
        exercise_backend(fixtures, 48000, benchmark_only);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << std::endl;
        result = EXIT_FAILURE;
    }
    MFShutdown();
    CoUninitialize();
    if (result == EXIT_SUCCESS) {
        std::cout << "DirectSound PCM backend tests passed" << std::endl;
    }
    return result;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << std::endl;
    return EXIT_FAILURE;
}
