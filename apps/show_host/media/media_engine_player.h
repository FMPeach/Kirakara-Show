#pragma once

#include "../host/host_utils.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <oleauto.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#pragma comment(lib, "mf")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "ole32")
#pragma comment(lib, "oleaut32")

// ── MediaEnginePlayer ────────────────────────────────────────────

struct MediaEnginePlayer {
    struct Notify final : IMFMediaEngineNotify {
        LONG ref_count{1};
        HWND window{};
        std::uint64_t generation{};

        Notify(HWND hwnd, std::uint64_t engine_generation)
            : window(hwnd), generation(engine_generation) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID riid, void** object) override {
            if (!object) return E_POINTER;
            *object = nullptr;
            if (riid == IID_IUnknown || riid == IID_IMFMediaEngineNotify) {
                *object = static_cast<IMFMediaEngineNotify*>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override {
            return static_cast<ULONG>(InterlockedIncrement(&ref_count));
        }

        ULONG STDMETHODCALLTYPE Release() override {
            const auto value = InterlockedDecrement(&ref_count);
            if (value == 0) delete this;
            return static_cast<ULONG>(value);
        }

        HRESULT STDMETHODCALLTYPE EventNotify(DWORD event, DWORD_PTR, DWORD) override {
            if (window) {
                PostMessageW(window, kVideoEventMessage, event,
                    static_cast<LPARAM>(generation));
            }
            return S_OK;
        }
    };

    IMFMediaEngine* engine{};
    Notify* notify{};
    bool loaded{};
    bool playing{};
    bool wants_playing{};
    bool has_pending_seek{};
    double pending_seek{};
    std::uint64_t generation{};
    HWND playback_window{};
    std::wstring source_path;

    ~MediaEnginePlayer() { close(); }

    void discard_pending_events(HWND window) {
        if (!window) return;
        MSG msg{};
        while (PeekMessageW(
                &msg, window, kVideoEventMessage, kVideoEventMessage,
                PM_REMOVE)) {
        }
    }

    void close() {
        const auto event_window = notify ? notify->window : nullptr;
        ++generation;
        if (notify) {
            notify->window = nullptr;
        }
        if (engine) {
            engine->Pause();
            engine->Shutdown();
        }
        release_com(engine);
        release_com(notify);
        loaded = false;
        playing = false;
        wants_playing = false;
        has_pending_seek = false;
        pending_seek = 0.0;
        playback_window = nullptr;
        source_path.clear();
        discard_pending_events(event_window);
    }

    bool open(HWND event_window, HWND video_window,
            const std::wstring& path) {
        close();

        if (!event_window || !video_window) return false;

        for (int attempt = 0; attempt < 3; ++attempt) {
            ++generation;
            HRESULT result = S_OK;
            IMFAttributes* attributes{};
            if (SUCCEEDED(result)) result = MFCreateAttributes(&attributes, 8);
            notify = new Notify(event_window, generation);
            if (SUCCEEDED(result)) {
                result = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
            }
            if (SUCCEEDED(result)) {
                result = attributes->SetUINT64(MF_MEDIA_ENGINE_PLAYBACK_HWND,
                    reinterpret_cast<UINT64>(video_window));
            }

            IMFMediaEngineClassFactory* factory{};
            if (SUCCEEDED(result)) {
                result = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr,
                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            }
            if (SUCCEEDED(result)) {
                result = factory->CreateInstance(
                    MF_MEDIA_ENGINE_REAL_TIME_MODE, attributes, &engine);
            }
            release_com(factory);
            release_com(attributes);

            if (SUCCEEDED(result)) {
                set_volume(0, false);
                engine->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
                const auto url = path.starts_with(L"http://")
                    || path.starts_with(L"https://")
                    ? path : file_url_from_path(path);
                BSTR source = SysAllocString(url.c_str());
                result = source
                    ? engine->SetSource(source) : E_OUTOFMEMORY;
                SysFreeString(source);
                if (FAILED(result)) {
                    log_hresult("MediaEnginePlayer::open: SetSource failed",
                        result);
                }
            }

            if (SUCCEEDED(result)) {
                loaded = true;
                playback_window = video_window;
                source_path = path;
                return true;
            }

            if (notify) { notify->window = nullptr; }
            if (engine) {
                engine->Pause();
                engine->Shutdown();
            }
            release_com(notify);
            release_com(engine);
            loaded = false;
            playing = false;
            discard_pending_events(event_window);
            Sleep(16 * (attempt + 1));
        }
        log_line("MediaEnginePlayer::open: failed after retries");
        return false;
    }

    bool open(HWND window, const std::wstring& path) {
        return open(window, window, path);
    }

    bool reopen_current(HWND event_window, HWND video_window,
            double seconds, bool should_play) {
        if (source_path.empty()) return false;
        const auto path = source_path;
        const auto opened = open(event_window, video_window, path);
        if (!opened) return false;
        if (std::isfinite(seconds) && seconds > 0.0) seek(seconds);
        if (should_play) play();
        return true;
    }

    bool reopen_current(HWND window, double seconds, bool should_play) {
        return reopen_current(window, window, seconds, should_play);
    }

    bool can_seek() const {
        return engine
            && engine->GetReadyState() >= MF_MEDIA_ENGINE_READY_HAVE_METADATA;
    }

    bool has_future_data() const {
        return engine
            && engine->GetReadyState()
                >= MF_MEDIA_ENGINE_READY_HAVE_FUTURE_DATA;
    }

    void apply_pending_seek() {
        if (!has_pending_seek || !can_seek()) return;
        if (SUCCEEDED(engine->SetCurrentTime(pending_seek))) {
            has_pending_seek = false;
        }
    }

    void try_play_if_requested() {
        if (!wants_playing || playing || !engine) return;
        apply_pending_seek();
        if (SUCCEEDED(engine->Play())) {
            playing = true;
        }
    }

    void play() {
        wants_playing = true;
        try_play_if_requested();
    }

    void pause() {
        wants_playing = false;
        if (engine) engine->Pause();
        playing = false;
    }

    void mark_ended() {
        playing = false;
        wants_playing = false;
    }

    void set_volume(int percent, bool audible) {
        if (!engine) return;
        if (audible) {
            const auto clamped = std::clamp(percent, 0, 100);
            engine->SetVolume(static_cast<double>(clamped) / 100.0);
            engine->SetMuted(FALSE);
        } else {
            engine->SetVolume(0.0);
            engine->SetMuted(TRUE);
        }
    }

    void seek(double seconds) {
        if (!engine) return;
        if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
        if (can_seek() && SUCCEEDED(engine->SetCurrentTime(seconds))) {
            has_pending_seek = false;
            pending_seek = seconds;
            return;
        }
        pending_seek = seconds;
        has_pending_seek = true;
    }

    void sync_to(double seconds) {
        if (!engine || !std::isfinite(seconds)) return;
        const auto current = current_time();
        if (!std::isfinite(current) || std::abs(current - seconds) > 0.18) {
            seek(seconds);
        }
    }

    double current_time() const {
        if (!engine) return 0.0;
        if (has_pending_seek) return pending_seek;
        const auto value = engine->GetCurrentTime();
        return std::isfinite(value) && value >= 0.0 ? value : 0.0;
    }

    double duration() const {
        if (!engine) return 0.0;
        const auto value = engine->GetDuration();
        return std::isfinite(value) && value > 0.0 ? value : 0.0;
    }

    void resize(RECT rect) {
        if (!engine) return;
        IMFMediaEngineEx* engine_ex{};
        if (SUCCEEDED(engine->QueryInterface(IID_PPV_ARGS(&engine_ex)))
                && engine_ex) {
            static_cast<void>(engine_ex->UpdateVideoStream(
                nullptr, &rect, nullptr));
        }
        release_com(engine_ex);
    }
};
