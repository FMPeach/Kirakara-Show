#include "kirakara/show/project_parser.hpp"
#include "kirakara/show/win32/lyric_renderer.hpp"
#include "kirakara/show/win32/song_title_renderer.hpp"
#include "kirakara/show/win32/d3d11_texture_surface.hpp"
#include "kirakara/show/config.hpp"

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
 
constexpr wchar_t kWindowClass[] = L"KirakaraShowVideoDemo";
constexpr wchar_t kSubtitleClass[] = L"KirakaraShowSubOverlay";
constexpr UINT kVideoEventMessage = WM_APP + 21;
constexpr wchar_t kLogPath[] = L"artifacts\\video-demo.log";

HINSTANCE g_instance{};

template <typename T>
void release_com(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

std::wstring format_time(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    const auto minutes = static_cast<int>(seconds) / 60;
    const auto remainder = seconds - minutes * 60;
    wchar_t buffer[64]{};
    swprintf(buffer, std::size(buffer), L"%02d:%05.2f", minutes, remainder);
    return buffer;
}

std::string read_file(const std::wstring& path) {
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    return {std::istreambuf_iterator<char>{stream},
        std::istreambuf_iterator<char>{}};
}

bool is_json_file(const std::wstring& path) {
    const auto extension = std::filesystem::path(path).extension().wstring();
    return _wcsicmp(extension.c_str(), L".json") == 0;
}

bool is_lrc_file(const std::wstring& path) {
    const auto extension = std::filesystem::path(path).extension().wstring();
    return _wcsicmp(extension.c_str(), L".lrc") == 0
        || _wcsicmp(extension.c_str(), L".txt") == 0
        || _wcsicmp(extension.c_str(), L".krl") == 0;
}

bool is_video_file(const std::wstring& path) {
    const auto extension = std::filesystem::path(path).extension().wstring();
    return _wcsicmp(extension.c_str(), L".mp4") == 0
        || _wcsicmp(extension.c_str(), L".mkv") == 0
        || _wcsicmp(extension.c_str(), L".mov") == 0
        || _wcsicmp(extension.c_str(), L".avi") == 0
        || _wcsicmp(extension.c_str(), L".webm") == 0;
}

std::vector<std::wstring> command_line_paths() {
    int argc{};
    auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> paths;
    std::wstring current;
    for (int i = 1; argv && i < argc; ++i) {
        if (!current.empty()) current += L" ";
        current += argv[i];
        if (is_lrc_file(current) || is_video_file(current)
                || std::filesystem::exists(current)) {
            paths.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) paths.push_back(current);
    if (argv) LocalFree(argv);
    return paths;
}

void append_log(const std::wstring& message) {
    std::wofstream stream(kLogPath, std::ios::app);
    if (stream) stream << message << L"\n";
}

std::wstring hresult_text(HRESULT result) {
    std::wstringstream stream;
    stream << L"0x" << std::hex << static_cast<unsigned long>(result);
    return stream.str();
}

std::wstring event_name(DWORD event) {
    switch (event) {
    case MF_MEDIA_ENGINE_EVENT_LOADSTART: return L"LOADSTART";
    case MF_MEDIA_ENGINE_EVENT_PROGRESS: return L"PROGRESS";
    case MF_MEDIA_ENGINE_EVENT_SUSPEND: return L"SUSPEND";
    case MF_MEDIA_ENGINE_EVENT_ABORT: return L"ABORT";
    case MF_MEDIA_ENGINE_EVENT_ERROR: return L"ERROR";
    case MF_MEDIA_ENGINE_EVENT_EMPTIED: return L"EMPTIED";
    case MF_MEDIA_ENGINE_EVENT_STALLED: return L"STALLED";
    case MF_MEDIA_ENGINE_EVENT_PLAY: return L"PLAY";
    case MF_MEDIA_ENGINE_EVENT_PAUSE: return L"PAUSE";
    case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA: return L"LOADEDMETADATA";
    case MF_MEDIA_ENGINE_EVENT_LOADEDDATA: return L"LOADEDDATA";
    case MF_MEDIA_ENGINE_EVENT_WAITING: return L"WAITING";
    case MF_MEDIA_ENGINE_EVENT_PLAYING: return L"PLAYING";
    case MF_MEDIA_ENGINE_EVENT_CANPLAY: return L"CANPLAY";
    case MF_MEDIA_ENGINE_EVENT_CANPLAYTHROUGH: return L"CANPLAYTHROUGH";
    case MF_MEDIA_ENGINE_EVENT_SEEKING: return L"SEEKING";
    case MF_MEDIA_ENGINE_EVENT_SEEKED: return L"SEEKED";
    case MF_MEDIA_ENGINE_EVENT_TIMEUPDATE: return L"TIMEUPDATE";
    case MF_MEDIA_ENGINE_EVENT_ENDED: return L"ENDED";
    case MF_MEDIA_ENGINE_EVENT_RATECHANGE: return L"RATECHANGE";
    case MF_MEDIA_ENGINE_EVENT_DURATIONCHANGE: return L"DURATIONCHANGE";
    case MF_MEDIA_ENGINE_EVENT_VOLUMECHANGE: return L"VOLUMECHANGE";
    case MF_MEDIA_ENGINE_EVENT_FORMATCHANGE: return L"FORMATCHANGE";
    case MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY: return L"FIRSTFRAMEREADY";
    case MF_MEDIA_ENGINE_EVENT_STREAMRENDERINGERROR: return L"STREAMRENDERINGERROR";
    default:
        return L"EVENT_" + std::to_wstring(event);
    }
}

std::wstring file_url_from_path(const std::wstring& path) {
    DWORD length = 4096;
    std::wstring url(length, L'\0');
    if (SUCCEEDED(UrlCreateFromPathW(path.c_str(), url.data(), &length, 0))) {
        url.resize(length);
        return url;
    }
    auto fallback = path;
    std::replace(fallback.begin(), fallback.end(), L'\\', L'/');
    return L"file:///" + fallback;
}

struct MediaEnginePlayer {
    struct Notify final : IMFMediaEngineNotify {
        LONG ref_count{1};
        HWND window{};
        std::uint64_t generation{};

        Notify(HWND hwnd, std::uint64_t engine_generation)
            : window(hwnd), generation(engine_generation) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
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
    DWORD last_event{};
    HRESULT last_result{S_OK};
    std::uint64_t generation{};

    ~MediaEnginePlayer() { close(); }

    void close() {
        if (notify) notify->window = nullptr;
        if (engine) {
            engine->Pause();
            engine->Shutdown();
        }
        release_com(engine);
        release_com(notify);
        loaded = false;
        playing = false;
        last_event = 0;
        last_result = S_OK;
    }

    bool open(HWND window, const std::wstring& path) {
        close();
        append_log(L"open path: " + path);
        ++generation;
        HRESULT result = S_OK;
        IMFAttributes* attributes{};
        if (SUCCEEDED(result)) result = MFCreateAttributes(&attributes, 8);
        notify = new Notify(window, generation);
        if (SUCCEEDED(result)) result = attributes->SetUnknown(
            MF_MEDIA_ENGINE_CALLBACK, notify);
        if (SUCCEEDED(result)) result = attributes->SetUINT64(
            MF_MEDIA_ENGINE_PLAYBACK_HWND,
            reinterpret_cast<UINT64>(window));
        IMFMediaEngineClassFactory* factory{};
        if (SUCCEEDED(result)) {
            result = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            append_log(L"CoCreateInstance factory: " + hresult_text(result));
        }
        if (SUCCEEDED(result)) {
            result = factory->CreateInstance(
                MF_MEDIA_ENGINE_REAL_TIME_MODE, attributes, &engine);
            append_log(L"CreateInstance: " + hresult_text(result));
        }
        release_com(factory);
        release_com(attributes);
        if (FAILED(result)) {
            close();
            return false;
        }

        engine->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
        const auto url = file_url_from_path(path);
        append_log(L"source url: " + url);
        BSTR source = SysAllocString(url.c_str());
        result = source ? engine->SetSource(source) : E_OUTOFMEMORY;
        SysFreeString(source);
        last_result = result;
        append_log(L"SetSource: " + hresult_text(result));
        if (FAILED(result)) {
            close();
            return false;
        }
        loaded = true;
        return true;
    }

    void play() {
        if (!engine) return;
        const auto result = engine->Play();
        last_result = result;
        append_log(L"Play: " + hresult_text(result));
        if (SUCCEEDED(result)) playing = true;
    }

    void pause() {
        if (engine) engine->Pause();
        playing = false;
    }

    void toggle() {
        if (!engine) return;
        if (playing || !engine->IsPaused()) pause(); else play();
    }

    double current_time() const {
        return engine ? engine->GetCurrentTime() : 0.0;
    }

    double duration() const {
        if (!engine) return 0.0;
        const auto value = engine->GetDuration();
        return std::isfinite(value) && value > 0.0 ? value : 0.0;
    }

    std::wstring media_error_text() const {
        if (!engine) return {};
        IMFMediaError* error{};
        if (FAILED(engine->GetError(&error)) || !error) return {};
        const auto code = error->GetErrorCode();
        release_com(error);
        return L" error=" + std::to_wstring(static_cast<int>(code));
    }

    void resize(RECT rect) {
        if (!engine) return;
        IMFMediaEngineEx* engine_ex{};
        if (SUCCEEDED(engine->QueryInterface(IID_PPV_ARGS(&engine_ex)))
                && engine_ex) {
            static_cast<void>(engine_ex->UpdateVideoStream(nullptr, &rect,
                nullptr));
        }
        release_com(engine_ex);
    }
};

struct AppState {
    HWND window{};
    HWND subtitle{};
    MediaEnginePlayer player;
    kirakara::show::PreparedDocument document;
    kirakara::show::EngineConfig config;
    kirakara::show::RenderStyle style;
    kirakara::show::SongTitleConfig song_title{
        kirakara::show::default_song_title_config()};
    kirakara::show::win32::LyricRenderer renderer;
    kirakara::show::win32::SongTitleRenderer title_renderer;
    kirakara::show::win32::D3D11TextureSurface surface;
    std::wstring path;
    std::wstring lyric_path;
    std::wstring last_event;
    double last_paint_time{-1.0};
    bool renderer_ready{};
    bool title_renderer_ready{};
    bool title_overlay_was_visible{};

    // Pre-allocated DIB for subtitle overlay (recreated on resize).
    HBITMAP dib{};
    void*  dib_bits{};
    std::uint32_t surf_w{};
    std::uint32_t surf_h{};

    void release_dib() {
        if (dib) { DeleteObject(dib); dib = nullptr; dib_bits = nullptr; }
    }
    ~AppState() { release_dib(); }

    // Fullscreen toggle.
    bool fullscreen{};
    RECT windowed_rect{};

    // FPS counter.
    double fps_smooth{};
    double fps_last_qpc{};
    double last_render_ms{};
    double last_readback_ms{};
    double last_ulw_ms{};
    double last_title_seconds{};
    double last_soft_sync_seconds{};
    bool render_loop_active{};

    // Continuous-time subtitle clock — drifts freely between periodic
    // soft syncs with the audio clock.
    double qpc_per_second{1.0};
    double subtitle_time_base{};
    LARGE_INTEGER qpc_base{};

    double subtitle_time() const {
        if (qpc_per_second <= 0.0) return player.current_time();
        // When paused, freeze at the video's current position so
        // subtitles don't drift while the user is reading them.
        if (player.engine && player.engine->IsPaused())
            return player.current_time();
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const auto elapsed = static_cast<double>(now.QuadPart - qpc_base.QuadPart)
            / qpc_per_second;
        return subtitle_time_base + elapsed;
    }

    void sync_subtitle_clock() {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        qpc_per_second = static_cast<double>(freq.QuadPart);
        subtitle_time_base = player.current_time();
        QueryPerformanceCounter(&qpc_base);
    }

    // Soft-nudge the subtitle clock toward the audio clock by at most
    // `max_correction` seconds, scaling with the elapsed time since
    // last sync.  Call this periodically (e.g. once per second).
    void nudge_subtitle_clock(double max_correction = 0.005) {
        const auto audio = player.current_time();
        const auto sub = subtitle_time();
        const auto drift = audio - sub;
        if (drift > max_correction)
            subtitle_time_base += max_correction;
        else if (drift < -max_correction)
            subtitle_time_base -= max_correction;
        else
            subtitle_time_base += drift; // within tolerance → full correction
    }
};

void ensure_title_renderer(AppState& app) {
    if (app.title_renderer_ready || !app.song_title.enabled) return;
    app.title_renderer_ready = app.title_renderer.initialize();
    if (!app.title_renderer_ready) {
        append_log(L"title renderer unavailable; continuing without title");
    }
}

bool title_overlay_active(const AppState& app, double media_time) {
    return app.title_renderer_ready
        && kirakara::show::song_title_has_drawable_content(app.song_title)
        && kirakara::show::song_title_visible(media_time, app.song_title);
}

bool load_lyrics(AppState& app, const std::wstring& path) {
    try {
        auto raw = read_file(path);
        auto base_config = kirakara::show::default_app_config();
        base_config.engine = app.config;
        base_config.style = app.style;
        base_config.font_families = app.renderer.font_families();
        if (!base_config.font_families.empty()) {
            base_config.font_family = base_config.font_families.front();
        }
        auto parsed = kirakara::show::parse_project_text(
            raw, std::move(base_config));
        if (!parsed.config_valid) {
            OutputDebugStringW(L"[video_demo] KRL config parse failed\n");
        }
        app.config = parsed.config.engine;
        app.style = parsed.config.style;
        app.song_title = std::move(parsed.config.song_title);
        app.renderer.set_font_families(parsed.config.font_families);
        app.document = std::move(parsed.document);
        ensure_title_renderer(app);
        app.lyric_path = path;
        app.last_paint_time = -1.0;
        append_log(L"loaded lyrics: " + path);
        return true;
    } catch (...) {
        append_log(L"failed to load lyrics: " + path);
        return false;
    }
}

bool load_video(AppState& app, const std::wstring& path) {
    if (!app.player.open(app.window, path)) return false;
    RECT rect{};
    GetClientRect(app.window, &rect);
    app.player.resize(rect);
    app.path = path;
    app.player.play();
    app.sync_subtitle_clock();
    append_log(L"loaded video: " + path);
    return true;
}

void seek(AppState& app, double delta) {
    if (!app.player.engine || app.player.duration() <= 0.0) return;
    auto target = app.player.current_time() + delta;
    target = std::clamp(target, 0.0, app.player.duration());
    app.player.engine->SetCurrentTime(target);
    app.sync_subtitle_clock();
}

void restart(AppState& app) {
    if (!app.player.engine || !app.player.loaded) return;
    app.player.engine->SetCurrentTime(0.0);
    app.player.engine->Play();
    app.player.playing = true;
    app.sync_subtitle_clock();
}

void toggle_fullscreen(AppState& app) {
    if (!app.window) return;
    if (!app.fullscreen) {
        GetWindowRect(app.window, &app.windowed_rect);
        SetWindowLongPtrW(app.window, GWL_STYLE,
            WS_POPUP | WS_VISIBLE);
        const auto screen_w = GetSystemMetrics(SM_CXSCREEN);
        const auto screen_h = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(app.window, HWND_TOP,
            0, 0, screen_w, screen_h,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        app.fullscreen = true;
    } else {
        SetWindowLongPtrW(app.window, GWL_STYLE,
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VISIBLE);
        SetWindowPos(app.window, nullptr,
            app.windowed_rect.left, app.windowed_rect.top,
            app.windowed_rect.right - app.windowed_rect.left,
            app.windowed_rect.bottom - app.windowed_rect.top,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        app.fullscreen = false;
    }
}

void update_title(AppState& app) {
    std::wstring title = L"Kirakara Video Demo - "
        + format_time(app.player.current_time());
    const auto duration = app.player.duration();
    if (duration > 0.0) title += L" / " + format_time(duration);
    if (!app.lyric_path.empty()) {
        title += L" - LRC: "
            + std::filesystem::path(app.lyric_path).filename().wstring();
    }
    wchar_t perf[96]{};
    swprintf(perf, std::size(perf), L" - %.1f FPS R%.2f rb%.2f",
        app.fps_smooth, app.last_render_ms, app.last_readback_ms);
    title += perf;
    if (!app.last_event.empty()) title += L" - " + app.last_event;
    title += L" | Space:暂停  ←→:±5s  R:重播  F5:重载配置";
    SetWindowTextW(app.window, title.c_str());
}

void paint_subtitle_overlay(AppState& app) {
    if (!app.subtitle) return;
    RECT client{};
    GetClientRect(app.subtitle, &client);
    const auto w = static_cast<std::uint32_t>(
        std::max(1L, client.right - client.left));
    const auto h = static_cast<std::uint32_t>(
        std::max(1L, client.bottom - client.top));
    if (w < 200 || h < 200) return;

    const auto time = app.subtitle_time();
    if (time == app.last_paint_time) return;
    app.last_paint_time = time;
    const bool should_paint_title = title_overlay_active(app, time);
    app.title_overlay_was_visible = should_paint_title;

    LARGE_INTEGER t0, t1, t2, t3, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    // GPU surface (only resize when window changes).
    if (!app.surface.resize(w, h)) return;

    // DIB cache (recreate only on size change).
    if (app.surf_w != w || app.surf_h != h || !app.dib) {
        app.release_dib();
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = static_cast<LONG>(w);
        info.bmiHeader.biHeight = -static_cast<LONG>(h);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        app.dib = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
            &app.dib_bits, nullptr, 0);
        if (!app.dib) return;
        app.surf_w = w;
        app.surf_h = h;
    }

    // Clear pass: transparent background.
    app.surface.begin_draw();
    app.surface.clear(kirakara::show::Color{0, 0, 0, 0});
    static_cast<void>(app.surface.end_draw());

    // Draw pass: render_overlay does its own BeginDraw/EndDraw.
    auto* rt = static_cast<ID2D1DeviceContext*>(
        app.surface.native_render_target());
    if (!rt) return;
    if (should_paint_title) {
        if (!app.title_renderer.attach_native_target(rt)) return;
        static_cast<void>(app.title_renderer.render(
            app.song_title, time, app.config.fade_duration));
    }
    if (!app.document.lines.empty()) {
        if (!app.renderer.attach_native_target(rt)) return;
        static_cast<void>(app.renderer.render_overlay(app.document,
            time, app.config, app.style));
    }
    QueryPerformanceCounter(&t1);

    // GPU→CPU: write BGRA directly into DIB (no swap, no intermediate buffer).
    if (!app.surface.read_bgra8_into(app.dib_bits)) return;

    QueryPerformanceCounter(&t2);
    const auto to_ms = [&](const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
        return static_cast<double>(b.QuadPart - a.QuadPart)
            * 1000.0 / static_cast<double>(freq.QuadPart);
    };
    const auto render_ms = to_ms(t0, t1);
    const auto readback_ms = to_ms(t1, t2);
    app.last_render_ms = render_ms;
    app.last_readback_ms = readback_ms;

    HDC screen_dc = GetDC(nullptr);
    HDC mem_dc = CreateCompatibleDC(screen_dc);
    const auto old_bmp = SelectObject(mem_dc, app.dib);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    SIZE size_dib{static_cast<LONG>(w), static_cast<LONG>(h)};
    POINT src{0, 0};

    SetBkMode(mem_dc, TRANSPARENT);
    SetTextColor(mem_dc, RGB(0, 255, 0));
    wchar_t fps_buf[96]{};
    swprintf(fps_buf, std::size(fps_buf), L"%.1f  R%.1f rb%.1f ulw%.1f",
        app.fps_smooth, render_ms, readback_ms, app.last_ulw_ms);
    TextOutW(mem_dc, 8, 8, fps_buf, static_cast<int>(wcslen(fps_buf)));
    if (app.dib_bits) {
        auto* pixels = static_cast<std::uint8_t*>(app.dib_bits);
        const auto max_y = std::min<std::uint32_t>(h, 34);
        const auto max_x = std::min<std::uint32_t>(w, 420);
        for (std::uint32_t py = 0; py < max_y; ++py) {
            for (std::uint32_t px = 0; px < max_x; ++px) {
                auto* bgra = pixels + (static_cast<std::size_t>(py) * w + px) * 4U;
                if (bgra[1] > 96 && bgra[0] < 96 && bgra[2] < 96) {
                    bgra[3] = 255;
                }
            }
        }
    }

    UpdateLayeredWindow(app.subtitle, screen_dc, nullptr, &size_dib,
        mem_dc, &src, 0, &blend, ULW_ALPHA);
    QueryPerformanceCounter(&t3);
    app.last_ulw_ms = to_ms(t2, t3);
    SelectObject(mem_dc, old_bmp);
    DeleteDC(mem_dc);
    ReleaseDC(nullptr, screen_dc);
}

void render_idle_frame(AppState& app) {
    LARGE_INTEGER qpc{};
    LARGE_INTEGER freq{};
    QueryPerformanceCounter(&qpc);
    QueryPerformanceFrequency(&freq);
    const auto now_seconds = static_cast<double>(qpc.QuadPart)
        / static_cast<double>(freq.QuadPart);
    if (app.fps_last_qpc > 0.0) {
        const auto dt = static_cast<double>(qpc.QuadPart - app.fps_last_qpc)
            / static_cast<double>(freq.QuadPart);
        if (dt > 0.0) {
            const auto instant = 1.0 / dt;
            constexpr double alpha = 0.1;
            app.fps_smooth = app.fps_smooth * (1.0 - alpha)
                + instant * alpha;
        }
    }
    app.fps_last_qpc = static_cast<double>(qpc.QuadPart);

    if (app.last_soft_sync_seconds <= 0.0
            || now_seconds - app.last_soft_sync_seconds >= 1.0) {
        app.last_soft_sync_seconds = now_seconds;
        app.nudge_subtitle_clock();
    }

    if (app.last_title_seconds <= 0.0
            || now_seconds - app.last_title_seconds >= 0.1) {
        app.last_title_seconds = now_seconds;
        update_title(app);
    }

    const auto media_time = app.subtitle_time();
    if (app.subtitle && app.renderer_ready
            && (!app.document.lines.empty()
                || title_overlay_active(app, media_time)
                || app.title_overlay_was_visible)) {
        paint_subtitle_overlay(app);
    }
}

LRESULT CALLBACK subtitle_proc(HWND hwnd, UINT message, WPARAM wparam,
    LPARAM lparam) {
    auto* app = reinterpret_cast<AppState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = reinterpret_cast<AppState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(app));
        return TRUE;
    }
    case WM_PAINT: {
        // WS_EX_LAYERED windows are drawn via UpdateLayeredWindow, not
        // via GDI BeginPaint/EndPaint.  Just validate and update.
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        if (app && app->renderer_ready) {
            paint_subtitle_overlay(*app);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam,
    LPARAM lparam) {
    auto* app = reinterpret_cast<AppState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = reinterpret_cast<AppState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->window = hwnd;
        return TRUE;
    }
    case WM_CREATE: {
        DragAcceptFiles(hwnd, TRUE);

        // Create a transparent overlay window for subtitles on top of
        // the video.  WS_EX_LAYERED + UpdateLayeredWindow gives us
        // per-pixel alpha so the video shows through everywhere except
        // where subtitle glyphs are drawn.
        const auto subtitle_hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
            kSubtitleClass, nullptr,
            WS_POPUP,
            0, 0, 100, 100, hwnd, nullptr, g_instance, app);
        if (subtitle_hwnd) {
            app->subtitle = subtitle_hwnd;
            ShowWindow(subtitle_hwnd, SW_SHOWNOACTIVATE);
        }
        app->render_loop_active = true;
        return 0;
    }
    case WM_SIZE:
        if (app) {
            RECT rect{};
            GetClientRect(hwnd, &rect);
            app->player.resize(rect);
            app->renderer.resize_window_target(LOWORD(lparam),
                HIWORD(lparam));
            if (app->subtitle) {
                // Reposition the subtitle overlay to exactly cover the
                // video window's client area.
                POINT top_left{rect.left, rect.top};
                ClientToScreen(hwnd, &top_left);
                SetWindowPos(app->subtitle, nullptr,
                    top_left.x, top_left.y,
                    rect.right - rect.left,
                    rect.bottom - rect.top,
                    SWP_NOACTIVATE | SWP_NOZORDER);
            }
        }
        return 0;
    case WM_MOVE:
        if (app && app->subtitle) {
            RECT rect{};
            GetClientRect(hwnd, &rect);
            POINT top_left{rect.left, rect.top};
            ClientToScreen(hwnd, &top_left);
            SetWindowPos(app->subtitle, nullptr,
                top_left.x, top_left.y,
                rect.right - rect.left,
                rect.bottom - rect.top,
                SWP_NOACTIVATE | SWP_NOZORDER);
        }
        return 0;
    case WM_DROPFILES: {
        if (!app) break;
        const auto drop = reinterpret_cast<HDROP>(wparam);
        const auto count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            wchar_t path[32768]{};
            DragQueryFileW(drop, i, path, std::size(path));
            if (is_json_file(path)) {
                auto preset = kirakara::show::default_app_config();
                if (kirakara::show::load_app_config(path, preset)) {
                    app->config = preset.engine;
                    app->style = preset.style;
                    app->renderer.set_font_families(preset.font_families);
                    if (!app->lyric_path.empty()) load_lyrics(*app, app->lyric_path);
                }
            } else if (is_lrc_file(path)) {
                if (!load_lyrics(*app, path)) {
                    MessageBoxW(hwnd, L"Could not load LRC file.",
                        L"Kirakara Video Demo", MB_ICONERROR);
                }
            } else if (is_video_file(path)) {
                if (!load_video(*app, path)) {
                    MessageBoxW(hwnd, L"Could not load video file.",
                        L"Kirakara Video Demo", MB_ICONERROR);
                }
            }
        }
        DragFinish(drop);
        update_title(*app);
        InvalidateRect(hwnd, nullptr, FALSE);
        if (app->subtitle) InvalidateRect(app->subtitle, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (app) {
            if (wparam == VK_SPACE) { app->player.toggle(); app->sync_subtitle_clock(); }
            else if (wparam == VK_RETURN) toggle_fullscreen(*app);
            else if (wparam == VK_ESCAPE && app->fullscreen) toggle_fullscreen(*app);
            else if (wparam == VK_LEFT) seek(*app, -5.0);
            else if (wparam == VK_RIGHT) seek(*app, 5.0);
            else if (wparam == 'R') restart(*app);
            else if (wparam == VK_F5) {
                auto preset = kirakara::show::default_app_config();
                const auto preset_path = std::filesystem::current_path()
                    / L"config" / L"kirakara-show-default.json";
                if (!kirakara::show::load_app_config(preset_path.wstring(), preset))
                    OutputDebugStringW(L"[video_demo] Config load failed\n");
                app->config = preset.engine;
                app->style = preset.style;
                app->renderer.set_font_families(preset.font_families);
                if (!app->lyric_path.empty()) load_lyrics(*app, app->lyric_path);
            }
        }
        return 0;
    case WM_ERASEBKGND:
        // Let D2D / MediaEngine own the background – do not let GDI erase.
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        [[maybe_unused]] auto paint_dc = BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        ValidateRect(hwnd, nullptr);
        return 0;
    }
    case kVideoEventMessage:
        if (app) {
            if (static_cast<std::uint64_t>(lparam) != app->player.generation) {
                return 0;
            }
            app->last_event = event_name(static_cast<DWORD>(wparam));
            if (static_cast<DWORD>(wparam) == MF_MEDIA_ENGINE_EVENT_ERROR) {
                app->last_event += app->player.media_error_text();
            }
            if (static_cast<DWORD>(wparam) == MF_MEDIA_ENGINE_EVENT_ENDED) {
                app->player.playing = false;
            }
            // Re-sync subtitle clock on playback state changes.
            if (static_cast<DWORD>(wparam) == MF_MEDIA_ENGINE_EVENT_PLAYING
                    || static_cast<DWORD>(wparam) == MF_MEDIA_ENGINE_EVENT_SEEKED) {
                app->sync_subtitle_clock();
            }
            append_log(L"event: " + app->last_event);
            update_title(*app);
            InvalidateRect(hwnd, nullptr, FALSE);
            if (app->subtitle) InvalidateRect(app->subtitle, nullptr, FALSE);
        }
        return 0;
    case WM_DESTROY:
        if (app) app->render_loop_active = false;
        if (app && app->subtitle) DestroyWindow(app->subtitle);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    g_instance = instance;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);
    DeleteFileW(kLogPath);

    const auto startup_paths = command_line_paths();

    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = kWindowClass;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(
        GetStockObject(BLACK_BRUSH));
    RegisterClassW(&window_class);

    WNDCLASSW sub_class{};
    sub_class.hInstance = instance;
    sub_class.lpfnWndProc = subtitle_proc;
    sub_class.lpszClassName = kSubtitleClass;
    sub_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    sub_class.hbrBackground = reinterpret_cast<HBRUSH>(
        GetStockObject(NULL_BRUSH));
    RegisterClassW(&sub_class);

    AppState app;
    auto preset = kirakara::show::default_app_config();
    const auto preset_path = std::filesystem::current_path()
        / L"config" / L"kirakara-show-default.json";
    static_cast<void>(kirakara::show::load_app_config(
        preset_path.wstring(), preset));
    app.config = preset.engine;
    app.style = preset.style;
    if (!app.renderer.initialize()) {
        MessageBoxW(nullptr, L"Could not initialize subtitle renderer.",
            L"Kirakara Video Demo", MB_ICONERROR);
        MFShutdown();
        CoUninitialize();
        return 1;
    }
    app.renderer.set_font_families(preset.font_families);
    const auto window = CreateWindowExW(0, kWindowClass,
        L"Kirakara Video Demo", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 760,
        nullptr, nullptr, instance, &app);
    if (!window) {
        MFShutdown();
        CoUninitialize();
        return 2;
    }
    ShowWindow(window, show);
    UpdateWindow(window);

    app.renderer_ready = true;
    for (const auto& startup_path : startup_paths) {
        if (is_json_file(startup_path)) {
            auto startup_config = kirakara::show::AppConfig{};
            startup_config.engine = app.config;
            startup_config.style = app.style;
            startup_config.font_families = app.renderer.font_families();
            if (!startup_config.font_families.empty()) {
                startup_config.font_family = startup_config.font_families.front();
            }
            if (kirakara::show::load_app_config(startup_path, startup_config)) {
                app.config = startup_config.engine;
                app.style = startup_config.style;
                app.song_title = std::move(startup_config.song_title);
                ensure_title_renderer(app);
                app.renderer.set_font_families(startup_config.font_families);
                if (!app.lyric_path.empty()) load_lyrics(app, app.lyric_path);
            }
        } else if (is_lrc_file(startup_path)) {
            load_lyrics(app, startup_path);
        } else if (is_video_file(startup_path)) {
            if (!load_video(app, startup_path)) {
                MessageBoxW(window, L"MediaEngine could not open the video.",
                    L"Kirakara Video Demo", MB_ICONERROR);
            }
        }
    }
    update_title(app);

    MSG message{};
    bool running = true;
    while (running) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running) break;
        if (app.render_loop_active && app.window) {
            render_idle_frame(app);
            Sleep(0);
        } else {
            WaitMessage();
        }
    }

    app.player.close();
    if (app.title_renderer_ready) app.title_renderer.shutdown();
    app.renderer.shutdown();
    MFShutdown();
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
