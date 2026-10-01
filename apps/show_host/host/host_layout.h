#pragma once

#include "host_data.h"
#include "host_render.h"
#include "host_utils.h"
#include "../show_host_api.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>


// Declarations for the definitions in host_layout.cpp

std::vector<std::shared_ptr<StageTextureSourceState>>
live_stage_texture_sources(ShowHost& host);

bool has_active_stage_texture_source(ShowHost& host);
bool has_active_external_texture_source(ShowHost& host);

void resize_media_player_for_stage(ShowHost& host, const RECT& stage_rect);

void attach_stage_window(ShowHost& host);

void apply_stage_layout(ShowHost& host);

bool set_stage_visible(ShowHost& host, bool visible);

void apply_stage_window_rect(
        ShowHost& host, const StageWindowRectRequest& request);

void resize_stage_surface(ShowHost& host);

void ensure_stage_surface_visible(ShowHost& host);
