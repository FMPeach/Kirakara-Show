#pragma once
#ifndef _WINSOCKAPI_
#include <winsock2.h>
#endif
#include "host_data.h"
#include "host_utils.h"
#include "host_render.h"
#include "host_layout.h"
#include "host_program.h"
#include "../stage/native_stage_session.h"
#include "../cast/aac_adts_packetizer.h"
#include "../cast/cast_av_sync.h"
#include "../cast/cast_session.h"
#include "../show_host_api.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>


constexpr auto kSeamlessTransitionHold = std::chrono::milliseconds(750);
constexpr auto kNativeVideoStartTimeout = std::chrono::seconds(5);
constexpr auto kCastVideoStartTimeout = std::chrono::seconds(5);
constexpr std::size_t kUnifiedStageFramePoolCapacity =
    kirakara::show::StageFramePool::default_capacity;
constexpr std::size_t kCastDecodedFrameCapacity = 16;
constexpr std::size_t kCastDecodedByteCapacity = 256U * 1024U * 1024U;
constexpr double kCastDecodeAheadSeconds = 0.25;

// Declarations for the definitions in host_commands.cpp

void render_idle_frame(ShowHost& host);
