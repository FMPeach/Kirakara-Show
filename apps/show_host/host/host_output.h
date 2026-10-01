#pragma once

#include "host_data.h"

#include <cstdint>

bool handle_start_cast_stream(ShowHost& host, std::uint16_t port);
void handle_stop_cast_stream(ShowHost& host);
