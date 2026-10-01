#pragma once

#include "host_data.h"
#include "host_utils.h"
#include "../show_host_api.h"
#include "kirakara/show/project_parser.hpp"

#include <windows.h>

#include <algorithm>
#include <utility>


// Declarations for the definitions in host_render.cpp

bool parse_lyric_project_data(
        const std::wstring& path,
        kirakara::show::PreparedDocument& document,
        kirakara::show::AppConfig& config);

bool parse_lyric_project(ShowHost& host, const std::wstring& path);

double media_time(const ShowHost& host);

double playback_time(const ShowHost& host);

void seek_video_outputs(ShowHost& host, double seconds);

bool should_suspend_native_video_for_cast(const ShowHost& host);

void play_video_outputs(ShowHost& host);

void pause_video_outputs(ShowHost& host);

void close_video_outputs(ShowHost& host);

void update_cached_position(ShowHost& host);

RECT fallback_stage_rect(HWND window);

RECT active_stage_rect(ShowHost& host);
