#pragma once

#include "../media/mf_d3d11_video_decoder.h"

[[nodiscard]] inline MfD3D11VideoDecoderConfig
native_stage_video_decoder_config() noexcept {
    MfD3D11VideoDecoderConfig config;
    config.observe_source_buffering_events = true;
    return config;
}

// The standby worker is promoted by swapping the complete decoder instance,
// including its immutable queue configuration, into the active slot. Paused
// state already limits preparation to the frame covering t=0; retaining the
// normal active queue here is what lets the promoted worker resume playback.
[[nodiscard]] inline MfD3D11VideoDecoderConfig
native_stage_standby_video_decoder_config() noexcept {
    auto config = native_stage_video_decoder_config();
    config.observe_local_source_growth = true;
    return config;
}
