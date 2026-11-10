#pragma once
#include "host_hud_renderer.h"
#include "lan_state_monitor.h"
#include "physical_hud_output.h"
#include "host_hud_controller.h"
#include "../show_host_api.h"
#include <atomic>

struct ShowHost;
struct HostHudRuntime {
    std::atomic<std::shared_ptr<const HostHudState>> state;
    std::atomic<std::shared_ptr<const HostHudQrImage>> qr;
    std::atomic<bool> lan_available{};
    std::atomic<std::uint64_t> lan_revision{};
    LanStateMonitor network;
    HostHudRenderer physical_renderer;
    PhysicalHudOutput physical_output;
    HostHudController controller;
    std::atomic<std::uint64_t> program_instance{}, discontinuity_revision{};
    std::atomic<bool> refresh_pending{};
    bool animation_timer_active{};
    ShowHostLanStateCallback callback{};
    void* callback_context{};
};
struct HostHudCallbackRequest { ShowHostLanStateCallback callback{}; void* context{}; };
void refresh_host_lan_state(ShowHost& host);
void request_host_hud_refresh(ShowHost& host);
void render_physical_hud(ShowHost& host);
void release_physical_hud(ShowHost& host);
