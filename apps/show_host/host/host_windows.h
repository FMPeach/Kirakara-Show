#pragma once
#include "host_data.h"
#include "host_render.h"
#include "host_layout.h"
#include "host_commands.h"
#include "host_output.h"
#include "host_utils.h"
#include "../show_host_api.h"
#include <windows.h>
#include <avrt.h>
#include <algorithm>
#include <chrono>
#include <cstdint>

namespace {

constexpr auto kStageRenderInterval = std::chrono::duration_cast<
    std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / 60.0));

class StageRenderWaiter {
public:
    StageRenderWaiter() {
        timer_ = CreateWaitableTimerExW(
            nullptr,
            nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS);
        if (!timer_) {
            timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        }
    }

    ~StageRenderWaiter() {
        if (timer_) CloseHandle(timer_);
    }

    void wait_until(std::chrono::steady_clock::time_point deadline) const {
        const auto now = std::chrono::steady_clock::now();
        if (deadline <= now) return;

        if (timer_) {
            const auto remaining = deadline - now;
            const auto ticks_100ns = std::max<std::int64_t>(
                1,
                std::chrono::duration_cast<
                    std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>>(
                        remaining).count());
            LARGE_INTEGER due{};
            due.QuadPart = -ticks_100ns;
            if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                const HANDLE handles[]{timer_};
                static_cast<void>(MsgWaitForMultipleObjectsEx(
                    1,
                    handles,
                    INFINITE,
                    QS_ALLINPUT,
                    MWMO_INPUTAVAILABLE));
                return;
            }
        }

        const auto timeout = std::max<DWORD>(
            1,
            static_cast<DWORD>(std::chrono::duration_cast<
                std::chrono::milliseconds>(deadline - now).count()));
        static_cast<void>(MsgWaitForMultipleObjectsEx(
            0,
            nullptr,
            timeout,
            QS_ALLINPUT,
            MWMO_INPUTAVAILABLE));
    }

private:
    HANDLE timer_{};
};

class StageThreadSchedulingScope {
public:
    StageThreadSchedulingScope() {
        avrt_module_ = LoadLibraryW(L"avrt.dll");
        if (avrt_module_) {
            set_characteristics_ = reinterpret_cast<SetCharacteristicsFn>(
                GetProcAddress(avrt_module_, "AvSetMmThreadCharacteristicsW"));
            set_priority_ = reinterpret_cast<SetPriorityFn>(
                GetProcAddress(avrt_module_, "AvSetMmThreadPriority"));
            revert_characteristics_ = reinterpret_cast<RevertCharacteristicsFn>(
                GetProcAddress(avrt_module_, "AvRevertMmThreadCharacteristics"));
            if (set_characteristics_ && revert_characteristics_) {
                DWORD task_index{};
                mmcss_handle_ = set_characteristics_(L"Playback", &task_index);
                if (mmcss_handle_) {
                    if (set_priority_) {
                        static_cast<void>(set_priority_(
                            mmcss_handle_, AVRT_PRIORITY_HIGH));
                    }
                    return;
                }
            }
        }

        previous_priority_ = GetThreadPriority(GetCurrentThread());
        if (previous_priority_ != THREAD_PRIORITY_ERROR_RETURN) {
            priority_changed_ = SetThreadPriority(
                GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL) != FALSE;
        }
    }

    ~StageThreadSchedulingScope() {
        if (mmcss_handle_ && revert_characteristics_) {
            static_cast<void>(revert_characteristics_(mmcss_handle_));
        } else if (priority_changed_) {
            static_cast<void>(SetThreadPriority(
                GetCurrentThread(), previous_priority_));
        }
        if (avrt_module_) FreeLibrary(avrt_module_);
    }

    StageThreadSchedulingScope(const StageThreadSchedulingScope&) = delete;
    StageThreadSchedulingScope& operator=(
        const StageThreadSchedulingScope&) = delete;

private:
    using SetCharacteristicsFn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
    using SetPriorityFn = BOOL(WINAPI*)(HANDLE, AVRT_PRIORITY);
    using RevertCharacteristicsFn = BOOL(WINAPI*)(HANDLE);

    HMODULE avrt_module_{};
    SetCharacteristicsFn set_characteristics_{};
    SetPriorityFn set_priority_{};
    RevertCharacteristicsFn revert_characteristics_{};
    HANDLE mmcss_handle_{};
    int previous_priority_{THREAD_PRIORITY_NORMAL};
    bool priority_changed_{};
};

}


// Declarations for the definitions in host_windows.cpp

LRESULT CALLBACK video_blank_proc(
    HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

LRESULT CALLBACK media_clock_proc(
    HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

LRESULT CALLBACK stage_proc(
    HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

void stage_thread_main(ShowHost* host);

ShowHost* as_host(ShowHostHandle handle);
