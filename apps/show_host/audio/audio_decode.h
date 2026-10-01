#pragma once

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <string>
#include <vector>

struct DecodedAudioBuffer;

// ── Ring buffer math ─────────────────────────────────────────────

std::size_t ring_distance_bytes(
    std::size_t from, std::size_t to, std::size_t ring_size);

// ── Audio decoding ───────────────────────────────────────────────

bool decode_audio_to_float_pcm(const std::wstring& path,
                               DecodedAudioBuffer& buffer);

// ── Resampling & channel conversion ──────────────────────────────

DecodedAudioBuffer resample_buffer(
    const DecodedAudioBuffer& src,
    int target_rate,
    int target_channels);

void downmix_to_stereo(DecodedAudioBuffer& buffer);
