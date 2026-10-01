#pragma once

#include "host_data.h"

#include <cstdint>
#include <memory>

double clamp_audio_clock_offset(double seconds);

void bump_playback_revision(ShowHost& host);
void bump_video_timeline_revision(ShowHost& host);

bool handle_stage_overlay_state(
    ShowHost& host, const StageOverlayRequest& request);

void begin_program_transition(ShowHost& host, const LoadRequest& request);
void fail_program_transition(ShowHost& host);
void expire_stalled_program_transition(ShowHost& host);

void apply_audio_processing(ShowHost& host);
bool open_media_engine_video(
    ShowHost& host, const std::wstring& source);

bool same_load_request(
    const LoadRequest& left, const LoadRequest& right);
bool handle_prepare_next(ShowHost& host, const LoadRequest& request);
std::unique_ptr<PreparedMediaPayload> take_prepared_media(
    ShowHost& host, const LoadRequest& request);
bool handle_load(ShowHost& host, const LoadRequest& request);

bool refresh_stage_program_ready(ShowHost& host);
void handle_play(ShowHost& host);
void handle_pause(ShowHost& host);
void handle_stop(ShowHost& host);
void handle_media_ended(ShowHost& host);
void handle_video_waiting(ShowHost& host);
void handle_video_can_play(ShowHost& host);
void maintain_video_master_sync(ShowHost& host);
void handle_seek(ShowHost& host, double seconds);

bool handle_audio_track(ShowHost& host, int track);
void handle_volume(ShowHost& host, int volume_percent);
void handle_key(ShowHost& host, int key_semitones);
void handle_audio_clock_offset(ShowHost& host, double seconds);
