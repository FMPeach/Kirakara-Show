#include "../apps/show_host/audio/audio_decode.h"
#include "../apps/show_host/audio/decoded_buffer.h"
#include "../apps/show_host/audio/opus_decode.h"

#include <windows.h>
#include <mfapi.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
void check_audio(const DecodedAudioBuffer& audio, size_t frames) {
    expect(audio.channels == 2 && audio.sample_rate == 48000, "48 kHz stereo Opus output");
    expect(audio.frame_count() == frames, "Opus pre-skip, tail trim, and chained duration");
    double energy = 0;
    for (auto sample : audio.samples) {
        expect(std::isfinite(sample), "finite decoded sample");
        energy += sample * sample;
    }
    expect(energy > 1, "audible Opus output");
}
}

int wmain(int argc, wchar_t** argv) {
    expect(argc == 2 || argc == 3, "pass fixture directory and optional HTTP base URL");
    const std::filesystem::path directory(argv[1]);
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    expect(SUCCEEDED(com), "COM initialization");
    expect(SUCCEEDED(MFStartup(MF_VERSION)), "Media Foundation startup");
    for (const auto* name : {L"mono.opus", L"stereo.opus", L"surround.opus", L"中文音轨.bin", L"chained.ogg"}) {
        DecodedAudioBuffer audio;
        expect(decode_audio_to_float_pcm((directory / name).wstring(), audio), "decode Opus through playback entry");
        check_audio(audio, std::wstring_view(name) == L"chained.ogg" ? 192000 : 96000);
    }
    for (const auto* name : {L"truncated.opus", L"damaged.opus", L"missing.opus"}) {
        DecodedAudioBuffer audio{1, 1, {1}};
        expect(!decode_audio_to_float_pcm((directory / name).wstring(), audio), "reject invalid Opus");
        expect(audio.samples.empty(), "failed decode must not expose partial audio");
    }
    for (const auto* name : {L"original.wav", L"original.m4a", L"original.mp3"}) {
        DecodedAudioBuffer audio;
        expect(decode_ogg_opus((directory / name).wstring(), audio) == OpusDecodeResult::not_opus,
            "other formats bypass libopus");
        expect(decode_audio_to_float_pcm((directory / name).wstring(), audio), "existing MF format still decodes");
        expect(audio.sample_rate == 44100 && audio.channels == 1, "preserve existing MF audio format");
        expect(std::abs(audio.duration() - 2) < 0.1, "existing MF duration");
    }
    if (argc == 3) {
        const std::wstring base(argv[2]);
        for (const auto* path : {L"/stereo.opus?token=test", L"/extensionless", L"/redirect.opus"}) {
            DecodedAudioBuffer audio;
            // Direct call exercises the extensionless URL fallback without
            // depending on the OS source resolver's unsupported-format delay.
            expect(decode_ogg_opus(base + path, audio, true) == OpusDecodeResult::decoded, "HTTP Opus decode");
            check_audio(audio, 96000);
        }
        DecodedAudioBuffer audio;
        expect(decode_audio_to_float_pcm(base + L"/stereo.opus", audio), "HTTP playback entry");
        check_audio(audio, 96000);
        expect(decode_audio_to_float_pcm(base + L"/extensionless", audio), "extensionless HTTP playback fallback");
        check_audio(audio, 96000);
        expect(decode_audio_to_float_pcm(base + L"/original.wav", audio), "existing HTTP MF format still decodes");
        expect(audio.channels == 1 && audio.sample_rate == 44100, "HTTP MF format unchanged");
        expect(decode_ogg_opus(base + L"/missing.opus", audio, true) == OpusDecodeResult::failed, "HTTP status validation");
    }
    MFShutdown();
    CoUninitialize();
    std::cout << "Opus local/HTTP, multichannel, gapless/chained, invalid input and unchanged MF formats passed\n";
}
