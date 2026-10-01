#include "kirakara/show/lrc_parser.hpp"
#include "kirakara/show/timeline.hpp"
#include "kirakara/show/win32/d3d11_texture_surface.hpp"
#include "kirakara/show/win32/offscreen_surface.hpp"
#include "kirakara/show/win32/lyric_renderer.hpp"
#include "kirakara/show/config.hpp"
#include "kirakara/show/win32/render_style.hpp"
#include "kirakara/show/win32/swap_chain_surface.hpp"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <d2d1.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numeric>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace kirakara::show;

namespace {

constexpr wchar_t kMainClass[] = L"KirakaraShowDemoWindow";
constexpr wchar_t kPreviewClass[] = L"KirakaraShowPreview";
constexpr UINT_PTR kFrameTimer = 1;
constexpr UINT kFrameTimerMs = 1; // uncapped preview polling
constexpr UINT kVideoEventMessage = WM_APP + 10;
constexpr bool kEnableMediaEngineVideo = false;

enum ControlId : int {
    id_load_lyrics = 100,
    id_load_audio,
    id_save_frame,
    id_play,
    id_timeline,
    id_font_size,
    id_stroke_width,
    id_letter_spacing,
    id_fade,
    id_indicator,
    id_ruby_isolate,
    id_role_colors,
    id_role_labels,
    id_role_combo,
    id_role_image_label,
    id_role_before_color,
    id_role_after_color,
    id_load_background,
    id_ruby_size_edit,
    id_ruby_offset_edit,
    id_line1_x_edit,
    id_line1_y_edit,
    id_line2_right_edit,
    id_line2_y_edit,
    id_font_family_combo,
    id_before_color,
    id_after_color,
    id_background_color,
    id_status,
    id_font_value,
    id_stroke_value,
    id_spacing_value,
};

std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) return {};
    const auto count = MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return L"?";
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), count);
    return result;
}

std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const auto count = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), count, nullptr, nullptr);
    return result;
}

std::string read_file(const std::wstring& path) {
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

std::wstring filename(const std::wstring& path) {
    return std::filesystem::path(path).filename().wstring();
}

bool is_image_file(const std::wstring& extension) {
    return _wcsicmp(extension.c_str(), L".png") == 0
        || _wcsicmp(extension.c_str(), L".jpg") == 0
        || _wcsicmp(extension.c_str(), L".jpeg") == 0
        || _wcsicmp(extension.c_str(), L".bmp") == 0;
}

bool is_video_file(const std::wstring& extension) {
    return _wcsicmp(extension.c_str(), L".mp4") == 0
        || _wcsicmp(extension.c_str(), L".mkv") == 0
        || _wcsicmp(extension.c_str(), L".mov") == 0
        || _wcsicmp(extension.c_str(), L".avi") == 0
        || _wcsicmp(extension.c_str(), L".webm") == 0;
}

bool is_lyric_file(const std::wstring& extension) {
    return _wcsicmp(extension.c_str(), L".lrc") == 0
        || _wcsicmp(extension.c_str(), L".txt") == 0
        || _wcsicmp(extension.c_str(), L".krl") == 0;
}

constexpr wchar_t kLyricFilter[] =
    L"Lyric files (*.lrc;*.txt;*.krl)\0*.lrc;*.txt;*.krl\0All files (*.*)\0*.*\0";
constexpr wchar_t kAudioFilter[] =
    L"Audio files (*.wav;*.mp3;*.m4a;*.flac)\0*.wav;*.mp3;*.m4a;*.flac\0"
    L"Video files (*.mp4;*.mkv;*.mov;*.avi;*.webm)\0*.mp4;*.mkv;*.mov;*.avi;*.webm\0"
    L"All files (*.*)\0*.*\0";
constexpr wchar_t kImageFilter[] =
    L"Image files (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0"
    L"All files (*.*)\0*.*\0";
constexpr wchar_t kBackgroundFilter[] =
    L"Background media (*.png;*.jpg;*.jpeg;*.bmp;*.mp4;*.mkv;*.mov;*.avi;*.webm)\0"
    L"*.png;*.jpg;*.jpeg;*.bmp;*.mp4;*.mkv;*.mov;*.avi;*.webm\0"
    L"Images (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0"
    L"Video files (*.mp4;*.mkv;*.mov;*.avi;*.webm)\0*.mp4;*.mkv;*.mov;*.avi;*.webm\0"
    L"All files (*.*)\0*.*\0";
constexpr wchar_t kPngSaveFilter[] =
    L"PNG image (*.png)\0*.png\0All files (*.*)\0*.*\0";

std::wstring format_time(double seconds) {
    seconds = std::max(0.0, seconds);
    const auto minutes = static_cast<int>(seconds) / 60;
    const auto remainder = seconds - minutes * 60;
    wchar_t buffer[64]{};
    swprintf(buffer, std::size(buffer), L"%02d:%05.2f", minutes, remainder);
    return buffer;
}

using TextPaint = kirakara::show::TextPaint;
using CharacterProfile = kirakara::show::CharacterProfile;
using DemoStyle = kirakara::show::RenderStyle;

template <typename T>
void release_com(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

TextPaint global_paint(const DemoStyle& style) {
    return {style.before, style.after, style.stroke_before, style.stroke_after};
}

TextPaint role_paint(std::string_view role, const DemoStyle& style) {
    if (!style.role_colors || role.empty()) return global_paint(style);
    if (const auto it = style.character_profiles.find(std::string{role});
            it != style.character_profiles.end()) {
        // Known role: profile → global → built-in (chain fallback, no palette).
        const auto& p = it->second;
        return {
            p.color_before  ? p.color_before  : style.before,
            p.color_after   ? p.color_after   : style.after,
            p.stroke_before ? p.stroke_before : style.stroke_before,
            p.stroke_after  ? p.stroke_after  : style.stroke_after,
            p.stroke_width > 0 ? p.stroke_width : style.stroke_width,
        };
    }
    // Role NOT in characterProfiles: random palette.
    // Only changes stroke_before and after (same colour); rest stays global.
    const auto c = kirakara::show::palette_color(role);
    return {
        style.before,        // before: global
        c,                   // after: palette
        c,                   // stroke_before: palette (same as after)
        style.stroke_after,  // stroke_after: global
        style.stroke_width,  // stroke_width: global
    };
}

struct AudioPlayer {
    bool loaded{};
    bool playing{};
    double duration{};
    double play_anchor_position{};
    std::chrono::steady_clock::time_point play_anchor_tick{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_mode_poll{};
    std::wstring temporary_wav;

    ~AudioPlayer() { close(); }

    void close() {
        mciSendStringW(L"close kk_audio", nullptr, 0, nullptr);
        if (!temporary_wav.empty()) {
            DeleteFileW(temporary_wav.c_str());
            temporary_wav.clear();
        }
        loaded = false;
        playing = false;
        duration = 0.0;
        play_anchor_position = 0.0;
        play_anchor_tick = std::chrono::steady_clock::now();
        last_mode_poll = {};
    }

    static bool transcode_to_wav(const std::wstring& input,
        std::wstring& output) {
        std::array<wchar_t, MAX_PATH> temp_directory{};
        if (GetTempPathW(static_cast<DWORD>(temp_directory.size()),
                temp_directory.data()) == 0) return false;
        output = temp_directory.data();
        output += L"KirakaraShow_" + std::to_wstring(GetCurrentProcessId()) + L".wav";
        auto command = L"ffmpeg.exe -y -v error -i \"" + input
            + L"\" -vn -acodec pcm_s16le \"" + output + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
            output.clear();
            return false;
        }
        const auto wait = WaitForSingleObject(process.hProcess, 120000);
        DWORD exit_code = 1;
        GetExitCodeProcess(process.hProcess, &exit_code);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (wait != WAIT_OBJECT_0 || exit_code != 0
                || GetFileAttributesW(output.c_str()) == INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(output.c_str());
            output.clear();
            return false;
        }
        return true;
    }

    static MCIERROR open_mci(const std::wstring& path) {
        auto command = L"open \"" + path + L"\" alias kk_audio";
        auto error = mciSendStringW(command.c_str(), nullptr, 0, nullptr);
        if (error != 0) {
            mciSendStringW(L"close kk_audio", nullptr, 0, nullptr);
            command = L"open \"" + path + L"\" type mpegvideo alias kk_audio";
            error = mciSendStringW(command.c_str(), nullptr, 0, nullptr);
        }
        return error;
    }

    bool open(const std::wstring& path) {
        close();
        auto error = open_mci(path);
        if (error != 0 && transcode_to_wav(path, temporary_wav)) {
            error = open_mci(temporary_wav);
        }
        if (error != 0) {
            if (!temporary_wav.empty()) {
                DeleteFileW(temporary_wav.c_str());
                temporary_wav.clear();
            }
            return false;
        }
        mciSendStringW(L"set kk_audio time format milliseconds", nullptr, 0, nullptr);
        loaded = true;
        play_anchor_position = 0.0;
        play_anchor_tick = std::chrono::steady_clock::now();
        last_mode_poll = {};
        wchar_t buffer[128]{};
        if (mciSendStringW(L"status kk_audio length", buffer, 128, nullptr) == 0) {
            duration = _wtof(buffer) / 1000.0;
        }
        return true;
    }

    void play() {
        if (!loaded) return;
        play_anchor_position = position();
        play_anchor_tick = std::chrono::steady_clock::now();
        mciSendStringW(L"play kk_audio", nullptr, 0, nullptr);
        playing = true;
        last_mode_poll = play_anchor_tick;
    }

    void pause() {
        if (!loaded) return;
        play_anchor_position = estimated_position();
        mciSendStringW(L"pause kk_audio", nullptr, 0, nullptr);
        playing = false;
        play_anchor_position = position();
        play_anchor_tick = std::chrono::steady_clock::now();
    }

    void seek(double seconds) {
        if (!loaded) return;
        play_anchor_position = std::clamp(seconds, 0.0, duration > 0.0 ? duration : seconds);
        play_anchor_tick = std::chrono::steady_clock::now();
        const auto command = L"seek kk_audio to "
            + std::to_wstring(static_cast<long long>(seconds * 1000.0));
        mciSendStringW(command.c_str(), nullptr, 0, nullptr);
        if (playing) mciSendStringW(L"play kk_audio", nullptr, 0, nullptr);
    }

    double position() {
        if (!loaded) return 0.0;
        wchar_t buffer[128]{};
        if (mciSendStringW(L"status kk_audio position", buffer, 128, nullptr) != 0) {
            return 0.0;
        }
        return _wtof(buffer) / 1000.0;
    }

    double estimated_position() const {
        if (!loaded) return 0.0;
        double value = play_anchor_position;
        if (playing) {
            value += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - play_anchor_tick).count();
        }
        if (duration > 0.0) value = std::min(value, duration);
        return std::max(0.0, value);
    }

    void refresh_mode() {
        if (!loaded) return;
        const auto now = std::chrono::steady_clock::now();
        if (last_mode_poll.time_since_epoch().count() != 0
                && now - last_mode_poll < std::chrono::milliseconds(250)) {
            return;
        }
        last_mode_poll = now;
        wchar_t buffer[64]{};
        if (mciSendStringW(L"status kk_audio mode", buffer, 64, nullptr) == 0) {
            const bool now_playing = std::wstring_view(buffer) == L"playing";
            if (playing && !now_playing) {
                play_anchor_position = position();
                play_anchor_tick = std::chrono::steady_clock::now();
            } else if (!playing && now_playing) {
                play_anchor_position = position();
                play_anchor_tick = std::chrono::steady_clock::now();
            }
            playing = now_playing;
        }
    }
};

struct VideoPlayer {
    struct Notify final : IMFMediaEngineNotify {
        LONG ref_count{1};
        HWND window{};

        explicit Notify(HWND hwnd) : window(hwnd) {}

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
            return InterlockedIncrement(&ref_count);
        }

        ULONG STDMETHODCALLTYPE Release() override {
            const auto value = InterlockedDecrement(&ref_count);
            if (value == 0) delete this;
            return value;
        }

        HRESULT STDMETHODCALLTYPE EventNotify(DWORD event, DWORD_PTR, DWORD) override {
            if (window) PostMessageW(window, kVideoEventMessage, event, 0);
            return S_OK;
        }
    };

    IMFMediaEngine* engine{};
    IMFDXGIDeviceManager* dxgi_manager{};
    Notify* notify{};
    UINT reset_token{};
    std::wstring path;
    bool loaded{};
    bool playing{};

    ~VideoPlayer() { close(); }

    void close() {
        if (engine) engine->Pause();
        release_com(engine);
        release_com(dxgi_manager);
        release_com(notify);
        reset_token = 0;
        path.clear();
        loaded = false;
        playing = false;
    }

    bool open(HWND event_window, void* native_d3d_device,
        const std::wstring& video_path) {
        close();
        if (!native_d3d_device) return false;

        auto* d3d_device = static_cast<ID3D11Device*>(native_d3d_device);
        auto result = MFCreateDXGIDeviceManager(&reset_token, &dxgi_manager);
        if (SUCCEEDED(result)) {
            result = dxgi_manager->ResetDevice(d3d_device, reset_token);
        }

        IMFAttributes* attributes{};
        if (SUCCEEDED(result)) result = MFCreateAttributes(&attributes, 6);
        notify = new Notify(event_window);
        if (SUCCEEDED(result)) {
            result = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
        }
        if (SUCCEEDED(result)) {
            result = attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER,
                dxgi_manager);
        }
        if (SUCCEEDED(result)) {
            result = attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT,
                DXGI_FORMAT_B8G8R8A8_UNORM);
        }

        IMFMediaEngineClassFactory* factory{};
        if (SUCCEEDED(result)) {
            result = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        }
        if (SUCCEEDED(result)) {
            result = factory->CreateInstance(0, attributes, &engine);
        }
        release_com(factory);
        release_com(attributes);
        if (FAILED(result)) {
            close();
            return false;
        }

        BSTR source = SysAllocString(video_path.c_str());
        result = source ? engine->SetSource(source) : E_OUTOFMEMORY;
        SysFreeString(source);
        if (FAILED(result)) {
            close();
            return false;
        }

        engine->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
        path = video_path;
        loaded = true;
        playing = false;
        return true;
    }

    bool play() {
        if (!engine) return false;
        if (SUCCEEDED(engine->Play())) {
            playing = true;
            return true;
        }
        return false;
    }

    void pause() {
        if (engine) engine->Pause();
        playing = false;
    }

    void toggle() {
        if (!loaded) return;
        if (playing) pause(); else play();
    }

    void seek(double seconds) {
        if (!engine) return;
        engine->SetCurrentTime(std::max(0.0, seconds));
    }

    double current_time() const {
        return engine ? engine->GetCurrentTime() : 0.0;
    }

    double duration() const {
        if (!engine) return 0.0;
        const auto value = engine->GetDuration();
        return std::isfinite(value) && value > 0.0 ? value : 0.0;
    }

    bool render_to(void* native_dxgi_surface, std::uint32_t width,
        std::uint32_t height) {
        if (!engine || !native_dxgi_surface || width == 0 || height == 0) {
            return false;
        }

        LONGLONG frame_time{};
        if (engine->OnVideoStreamTick(&frame_time) != S_OK) return false;

        DWORD video_width{};
        DWORD video_height{};
        if (FAILED(engine->GetNativeVideoSize(&video_width, &video_height))
                || video_width == 0 || video_height == 0) {
            video_width = 16;
            video_height = 9;
        }
        const auto scale = std::min(
            static_cast<double>(width) / static_cast<double>(video_width),
            static_cast<double>(height) / static_cast<double>(video_height));
        const auto draw_width = static_cast<LONG>(
            std::round(static_cast<double>(video_width) * scale));
        const auto draw_height = static_cast<LONG>(
            std::round(static_cast<double>(video_height) * scale));
        RECT destination{
            static_cast<LONG>((static_cast<LONG>(width) - draw_width) / 2),
            static_cast<LONG>((static_cast<LONG>(height) - draw_height) / 2),
            static_cast<LONG>((static_cast<LONG>(width) + draw_width) / 2),
            static_cast<LONG>((static_cast<LONG>(height) + draw_height) / 2),
        };
        MFARGB black{255, 0, 0, 0};
        auto* surface = static_cast<IDXGISurface*>(native_dxgi_surface);
        return SUCCEEDED(engine->TransferVideoFrame(
            static_cast<IUnknown*>(surface), nullptr, &destination, &black));
    }
};

struct AppState {
    HINSTANCE instance{};
    HWND window{};
    HWND preview{};
    HWND timeline{};
    HWND status{};
    HWND play_button{};
    HWND font_value{};
    HWND stroke_value{};
    HWND spacing_value{};
    HWND role_combo{};
    PreparedDocument document;
    EngineConfig config;
    DemoStyle style;
    win32::SwapChainSurface preview_surface;
    AudioPlayer audio;
    VideoPlayer video;
    std::wstring lyric_path;
    std::wstring audio_path;
    std::wstring background_path;
    double current_time{};
    double duration{15.0};
    bool local_playing{};
    bool dragging_timeline{};
    bool preview_target_attached{};
    std::chrono::steady_clock::time_point local_tick{std::chrono::steady_clock::now()};

    void update_duration() {
        const auto lyric_end = document.lines.empty() ? 15.0
            : document.lines.back().end_time + config.exit_buffer;
        duration = std::max({1.0, lyric_end, audio.duration, video.duration()});
        SendMessageW(timeline, TBM_SETRANGEMAX, FALSE,
            static_cast<LPARAM>(std::max(1, static_cast<int>(duration * 1000.0))));
    }

    void load_builtin() {
        constexpr std::string_view sample =
            "[A][00:01.00]{Ki|ki}[00:01.50]{ra|ra}[00:02.00]"
            "{ka|ka}[00:02.50]{ra|ra}[00:03.00] Show[00:04.50]\n"
            "[B][00:05.00]Real[00:05.50]time[00:06.00] "
            "karaoke[00:07.00] subtitle[00:08.00] demo[00:09.00]\n";
        document = parse_lrc(sample, config).document;
        update_duration();
    }
};

win32::LyricRenderer g_renderer;

void set_text(HWND control, const std::wstring& text) {
    SetWindowTextW(control, text.c_str());
}

HWND make_control(HWND parent, const wchar_t* class_name, const wchar_t* text,
    DWORD style, int x, int y, int width, int height, int id) {
    return CreateWindowExW(0, class_name, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
}

void add_label(HWND parent, const wchar_t* text, int x, int y, int width = 120) {
    make_control(parent, L"STATIC", text, SS_LEFT, x, y, width, 22, 0);
}

int edit_integer(HWND window, int id, int fallback) {
    wchar_t value[32]{};
    GetDlgItemTextW(window, id, value, static_cast<int>(std::size(value)));
    wchar_t* end{};
    const auto parsed = wcstol(value, &end, 10);
    return end != value ? static_cast<int>(parsed) : fallback;
}

void update_value_labels(AppState& app) {
    set_text(app.font_value, std::to_wstring(static_cast<int>(app.style.font_size)) + L" px");
    set_text(app.stroke_value, std::to_wstring(static_cast<int>(app.style.stroke_width)) + L" px");
    set_text(app.spacing_value, std::to_wstring(static_cast<int>(app.style.letter_spacing)) + L" px");
}

void update_status(AppState& app) {
    std::wstring text = format_time(app.current_time) + L" / " + format_time(app.duration);
    text += L"    Lyrics: " + (app.lyric_path.empty() ? L"built-in" : filename(app.lyric_path));
    text += L"    Audio: " + (app.audio_path.empty() ? L"not loaded" : filename(app.audio_path));
    if (app.video.loaded) {
        text += L"    Video: " + filename(app.video.path);
    }
    set_text(app.status, text);
    set_text(app.play_button,
        app.video.playing || app.audio.playing || app.local_playing ? L"Pause" : L"Play");
}

void refresh_role_combo(AppState& app) {
    if (!app.role_combo) return;
    std::wstring previous;
    const auto old_selection = static_cast<int>(SendMessageW(
        app.role_combo, CB_GETCURSEL, 0, 0));
    if (old_selection >= 0) {
        const auto length = static_cast<int>(SendMessageW(
            app.role_combo, CB_GETLBTEXTLEN, old_selection, 0));
        previous.resize(static_cast<std::size_t>(std::max(0, length)));
        if (length > 0) SendMessageW(app.role_combo, CB_GETLBTEXT,
            old_selection, reinterpret_cast<LPARAM>(previous.data()));
    }
    SendMessageW(app.role_combo, CB_RESETCONTENT, 0, 0);
    std::set<std::string> roles;
    for (const auto& line : app.document.lines) {
        for (const auto& character : line.chars) {
            roles.insert(character.roles.begin(), character.roles.end());
        }
    }
    int selection = 0;
    int index = 0;
    for (const auto& role : roles) {
        const auto wide = utf8_to_wide(role);
        SendMessageW(app.role_combo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(wide.c_str()));
        if (!previous.empty() && wide == previous) selection = index;
        ++index;
    }
    if (!roles.empty()) SendMessageW(app.role_combo, CB_SETCURSEL, selection, 0);
}

std::string selected_role(const AppState& app) {
    if (!app.role_combo) return {};
    const auto selection = static_cast<int>(SendMessageW(
        app.role_combo, CB_GETCURSEL, 0, 0));
    if (selection < 0) return {};
    const auto length = static_cast<int>(SendMessageW(
        app.role_combo, CB_GETLBTEXTLEN, selection, 0));
    if (length <= 0) return {};
    std::wstring value(static_cast<std::size_t>(length), L'\0');
    SendMessageW(app.role_combo, CB_GETLBTEXT, selection,
        reinterpret_cast<LPARAM>(value.data()));
    return wide_to_utf8(value);
}

std::wstring choose_file(HWND owner, const wchar_t* filter) {
    std::array<wchar_t, 32768> path{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameW(&dialog) ? std::wstring(path.data()) : std::wstring{};
}

std::wstring choose_save_png(HWND owner) {
    std::array<wchar_t, 32768> path{};
    wcscpy_s(path.data(), path.size(), L"kirakara-frame.png");
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = kPngSaveFilter;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrDefExt = L"png";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    return GetSaveFileNameW(&dialog) ? std::wstring(path.data()) : std::wstring{};
}

bool load_background_media(HWND owner, AppState& app, const std::wstring& path) {
    const auto extension = std::filesystem::path(path).extension().wstring();
    if (is_image_file(extension)) {
        app.video.close();
        app.background_path = path;
        InvalidateRect(app.preview, nullptr, FALSE);
        return true;
    }
    if (is_video_file(extension)) {
        if (!kEnableMediaEngineVideo) {
            MessageBoxW(owner,
                L"Video background is temporarily disabled because the "
                L"MediaEngine compositor path can hang the demo. The subtitle "
                L"preview remains available; video will be re-enabled after "
                L"the compositor is moved out of the UI paint path.",
                L"Video disabled", MB_ICONINFORMATION);
            return false;
        }
        if (!app.preview_surface.native_render_target()
                && !app.preview_surface.initialize(app.preview)) {
            MessageBoxW(owner, L"Could not initialize the video preview surface.",
                L"Video load failed", MB_ICONERROR);
            return false;
        }
        if (!app.video.open(app.window, app.preview_surface.native_d3d_device(),
                path)) {
            MessageBoxW(owner, L"MediaEngine could not open this video.",
                L"Video load failed", MB_ICONERROR);
            return false;
        }
        app.background_path.clear();
        app.audio.close();
        app.audio_path.clear();
        app.current_time = 0.0;
        app.update_duration();
        app.background_path.clear();
        InvalidateRect(app.preview, nullptr, FALSE);
        return true;
    }
    MessageBoxW(owner, L"Unsupported background file type.",
        L"Kirakara Show", MB_ICONWARNING);
    return false;
}

bool save_current_frame(const AppState& app, std::wstring_view path) {
    win32::OffscreenSurface surface;
    if (!surface.resize(1280, 720)) return false;
    win32::LyricRenderer renderer;
    if (!renderer.initialize()) return false;
    renderer.set_font_families(g_renderer.font_families());
    return renderer.attach_native_target(surface.native_render_target())
        && renderer.render(app.document, app.current_time,
            app.config, app.style)
        && surface.save_png(path);
}

bool benchmark_renderer(const AppState& app, int frame_count,
    std::wstring_view json_path, bool use_gpu, double target_rate) {
    if (frame_count <= 0) return false;
    win32::OffscreenSurface surface;
    win32::D3D11TextureSurface gpu_surface;
    void* native_target{};
    if (use_gpu) {
        if (!gpu_surface.resize(1920, 1080)) return false;
        native_target = gpu_surface.native_render_target();
    } else {
        if (!surface.resize(1920, 1080)) return false;
        native_target = surface.native_render_target();
    }
    win32::LyricRenderer renderer;
    if (!renderer.initialize()) return false;
    renderer.set_font_families(g_renderer.font_families());
    if (!renderer.attach_native_target(native_target)) return false;

    constexpr int warmup_frames = 30;
    const auto playback_rate = target_rate > 0.0 ? target_rate : 120.0;
    const auto benchmark_start = app.current_time > 0.0 ? app.current_time
        : (app.document.lines.empty() ? 1.0
            : std::max(0.0, app.document.lines.front().start_time + 0.25));
    const auto benchmark_end = app.document.lines.empty() ? app.duration
        : std::max(benchmark_start + 1.0,
            app.document.lines.back().end_time + app.config.exit_buffer);
    const auto render_frame = [&](int index) {
        const auto span = std::max(1.0, benchmark_end - benchmark_start);
        const auto time = benchmark_start
            + std::fmod(static_cast<double>(index) / playback_rate, span);
        return renderer.render(app.document, time, app.config, app.style);
    };
    for (int i = 0; i < warmup_frames; ++i) {
        if (!render_frame(i)) return false;
    }
    std::vector<double> frame_times_ms;
    std::vector<double> lateness_ms;
    frame_times_ms.reserve(static_cast<std::size_t>(frame_count));
    lateness_ms.reserve(static_cast<std::size_t>(frame_count));
    const bool paced = target_rate > 0.0;
    const auto target_period = paced
        ? std::chrono::duration<double>(1.0 / target_rate)
        : std::chrono::duration<double>::zero();
    const auto started = std::chrono::steady_clock::now();
    int deadline_misses = 0;
    for (int i = 0; i < frame_count; ++i) {
        const auto scheduled_start = started
            + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                target_period * i);
        const auto deadline = scheduled_start
            + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                target_period);
        if (paced) {
            std::this_thread::sleep_until(scheduled_start);
        }
        const auto frame_started = std::chrono::steady_clock::now();
        if (!render_frame(i + warmup_frames)) return false;
        const auto frame_finished = std::chrono::steady_clock::now();
        frame_times_ms.push_back(std::chrono::duration<double, std::milli>(
            frame_finished - frame_started).count());
        if (paced) {
            const auto late = std::chrono::duration<double, std::milli>(
                frame_finished - deadline).count();
            lateness_ms.push_back(std::max(0.0, late));
            if (late > 0.0) ++deadline_misses;
        }
    }
    if (use_gpu && !gpu_surface.synchronize()) return false;
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    const auto average_ms = elapsed * 1000.0 / static_cast<double>(frame_count);
    const auto fps = static_cast<double>(frame_count) / elapsed;
    auto sorted_frame_times = frame_times_ms;
    std::sort(sorted_frame_times.begin(), sorted_frame_times.end());
    const auto percentile_from = [](const std::vector<double>& values, double p) {
        if (values.empty()) return 0.0;
        const auto index = static_cast<std::size_t>(std::clamp(
            p * static_cast<double>(values.size() - 1), 0.0,
            static_cast<double>(values.size() - 1)));
        return values[index];
    };
    auto sorted_lateness = lateness_ms;
    std::sort(sorted_lateness.begin(), sorted_lateness.end());
    const auto percentile = [&](double p) {
        return percentile_from(sorted_frame_times, p);
    };
    const auto min_ms = sorted_frame_times.empty() ? 0.0 : sorted_frame_times.front();
    const auto max_ms = sorted_frame_times.empty() ? 0.0 : sorted_frame_times.back();
    const auto submit_average_ms = frame_times_ms.empty() ? 0.0
        : std::accumulate(frame_times_ms.begin(), frame_times_ms.end(), 0.0)
            / static_cast<double>(frame_times_ms.size());
    const auto target_budget_ms = paced ? 1000.0 / target_rate : 0.0;
    const auto miss_ratio = paced
        ? static_cast<double>(deadline_misses) / static_cast<double>(frame_count)
        : 0.0;
    const auto submit_over_budget = paced
        ? static_cast<int>(std::count_if(frame_times_ms.begin(), frame_times_ms.end(),
            [&](double value) { return value > target_budget_ms; }))
        : 0;
    const auto submit_over_budget_ratio = paced
        ? static_cast<double>(submit_over_budget) / static_cast<double>(frame_count)
        : 0.0;

    if (!json_path.empty()) {
        std::ofstream output(std::filesystem::path{json_path},
            std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << "{\n"
            << "  \"backend\": \"" << (use_gpu ? "d3d11" : "wic") << "\",\n"
            << "  \"hardware_device\": "
            << (use_gpu && gpu_surface.using_hardware_device() ? "true" : "false")
            << ",\n"
            << "  \"width\": 1920,\n"
            << "  \"height\": 1080,\n"
            << "  \"warmup_frames\": " << warmup_frames << ",\n"
            << "  \"measured_frames\": " << frame_count << ",\n"
            << "  \"benchmark_start_seconds\": " << benchmark_start << ",\n"
            << "  \"benchmark_end_seconds\": " << benchmark_end << ",\n"
            << "  \"target_frame_rate\": " << target_rate << ",\n"
            << "  \"target_frame_budget_ms\": " << target_budget_ms << ",\n"
            << "  \"elapsed_seconds\": " << elapsed << ",\n"
            << "  \"average_frame_ms\": " << average_ms << ",\n"
            << "  \"average_submit_frame_ms\": " << submit_average_ms << ",\n"
            << "  \"submit_frame_ms_min\": " << min_ms << ",\n"
            << "  \"submit_frame_ms_p50\": " << percentile(0.50) << ",\n"
            << "  \"submit_frame_ms_p95\": " << percentile(0.95) << ",\n"
            << "  \"submit_frame_ms_p99\": " << percentile(0.99) << ",\n"
            << "  \"submit_frame_ms_max\": " << max_ms << ",\n"
            << "  \"submit_over_budget_frames\": " << submit_over_budget << ",\n"
            << "  \"submit_over_budget_ratio\": " << submit_over_budget_ratio << ",\n"
            << "  \"deadline_miss_frames\": " << deadline_misses << ",\n"
            << "  \"deadline_miss_ratio\": " << miss_ratio << ",\n"
            << "  \"deadline_lateness_ms_p95\": "
            << percentile_from(sorted_lateness, 0.95) << ",\n"
            << "  \"deadline_lateness_ms_p99\": "
            << percentile_from(sorted_lateness, 0.99) << ",\n"
            << "  \"deadline_lateness_ms_max\": "
            << (sorted_lateness.empty() ? 0.0 : sorted_lateness.back()) << ",\n"
            << "  \"frames_per_second\": " << fps << "\n"
            << "}\n";
        if (!output) return false;
    }
    return true;
}

bool choose_color(HWND owner, kirakara::show::PackedColor& color) {
    static std::array<COLORREF, 16> custom{};
    CHOOSECOLORW dialog{sizeof(dialog)};
    dialog.hwndOwner = owner;
    dialog.rgbResult = static_cast<COLORREF>(color);
    dialog.lpCustColors = custom.data();
    dialog.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&dialog)) return false;
    color = static_cast<kirakara::show::PackedColor>(dialog.rgbResult);
    return true;
}

std::string lyric_text_with_project_config(AppState& app) {
    auto raw = read_file(app.lyric_path);
    const std::string cfg_marker = "config {";
    const auto cfg_pos = raw.find(cfg_marker);
    if (cfg_pos != std::string::npos) {
        const auto brace = raw.find('{', cfg_pos + cfg_marker.size() - 1);
        if (brace != std::string::npos) {
            std::size_t depth = 1;
            std::size_t end = brace + 1;
            while (end < raw.size() && depth > 0) {
                if (raw[end] == '{') ++depth;
                else if (raw[end] == '}') --depth;
                ++end;
            }
            if (depth == 0) {
                AppConfig cfg;
                cfg.engine = app.config;
                cfg.style = app.style;
                cfg.font_families = g_renderer.font_families();
                if (!cfg.font_families.empty()) {
                    cfg.font_family = cfg.font_families.front();
                }
                const auto cfg_json = raw.substr(brace, end - brace);
                if (load_app_config_json(cfg_json, cfg)) {
                    app.config = cfg.engine;
                    app.style = cfg.style;
                    g_renderer.set_font_families(cfg.font_families);
                }
                raw.erase(cfg_pos, end - cfg_pos);
            }
        }
    }

    auto role_pos = raw.find("role {");
    while (role_pos != std::string::npos) {
        const auto brace = raw.find('{', role_pos + 4);
        if (brace == std::string::npos) break;
        std::size_t depth = 1;
        std::size_t end = brace + 1;
        while (end < raw.size() && depth > 0) {
            if (raw[end] == '{') ++depth;
            else if (raw[end] == '}') --depth;
            ++end;
        }
        if (depth != 0) break;
        raw.erase(role_pos, end - role_pos);
        role_pos = raw.find("role {");
    }
    return raw;
}

void rebuild_timeline(AppState& app) {
    if (app.lyric_path.empty()) {
        app.load_builtin();
    } else {
        const auto parsed = parse_lrc(lyric_text_with_project_config(app),
            app.config);
        app.document = parsed.document;
        app.update_duration();
    }
    refresh_role_combo(app);
    InvalidateRect(app.preview, nullptr, FALSE);
}

bool ensure_preview_target(AppState& app, HWND hwnd) {
    if (!app.preview_surface.native_render_target()) {
        if (!app.preview_surface.initialize(hwnd)) return false;
        app.preview_target_attached = false;
    }
    if (!app.preview_target_attached) {
        app.preview_target_attached = g_renderer.attach_native_target(
            app.preview_surface.native_render_target());
    }
    return app.preview_target_attached;
}

LRESULT CALLBACK preview_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        if (app && ensure_preview_target(*app, hwnd)) {
            if (app->video.loaded) {
                static_cast<void>(app->video.render_to(
                    app->preview_surface.native_dxgi_surface(),
                    app->preview_surface.width(), app->preview_surface.height()));
                app->current_time = app->video.current_time();
            }
            auto rendered = app->video.loaded
                ? g_renderer.render_overlay(
                    app->document, app->current_time, app->config, app->style)
                : g_renderer.render(
                    app->document, app->current_time, app->config, app->style);
            if (!rendered) {
                app->preview_target_attached = false;
                if (ensure_preview_target(*app, hwnd)) {
                    rendered = app->video.loaded
                        ? g_renderer.render_overlay(
                            app->document, app->current_time,
                            app->config, app->style)
                        : g_renderer.render(
                            app->document, app->current_time,
                            app->config, app->style);
                }
            }
            if (rendered) static_cast<void>(app->preview_surface.present(true));
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (app && app->preview_surface.native_render_target()) {
            if (!app->preview_surface.resize(LOWORD(lparam), HIWORD(lparam))) {
                app->preview_target_attached = false;
            }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void layout_controls(AppState& app, int width, int height) {
    constexpr int panel = 280;
    MoveWindow(app.preview, panel + 8, 8, std::max(100, width - panel - 16),
        std::max(100, height - 88), TRUE);
    MoveWindow(app.timeline, panel + 8, height - 68, std::max(100, width - panel - 16), 30, TRUE);
    MoveWindow(app.status, panel + 8, height - 35, std::max(100, width - panel - 16), 24, TRUE);
}

LRESULT CALLBACK main_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = reinterpret_cast<AppState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->window = hwnd;
        return TRUE;
    }
    case WM_CREATE: {
        SetWindowTextW(hwnd, L"Kirakara Show - C++ realtime preview");
        constexpr int x = 14;
        const auto int_text = [](float value) {
            return std::to_wstring(static_cast<int>(std::round(value)));
        };
        app->preview = CreateWindowExW(0, kPreviewClass, L"", WS_CHILD | WS_VISIBLE,
            290, 8, 800, 600, hwnd, nullptr, app->instance, app);
        make_control(hwnd, L"BUTTON", L"Load LRC", BS_PUSHBUTTON, x, 14, 78, 32, id_load_lyrics);
        make_control(hwnd, L"BUTTON", L"Load Audio", BS_PUSHBUTTON, x + 86, 14, 78, 32, id_load_audio);
        make_control(hwnd, L"BUTTON", L"Save PNG", BS_PUSHBUTTON, x + 172, 14, 78, 32, id_save_frame);
        app->play_button = make_control(hwnd, L"BUTTON", L"Play", BS_PUSHBUTTON,
            x, 54, 250, 34, id_play);

        add_label(hwnd, L"Font size", x, 108);
        const auto font_value_text = int_text(app->style.font_size) + L" px";
        app->font_value = make_control(hwnd, L"STATIC", font_value_text.c_str(),
            SS_RIGHT, 190, 108, 70, 22, id_font_value);
        const auto font = make_control(hwnd, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS,
            x, 132, 246, 34, id_font_size);
        SendMessageW(font, TBM_SETRANGE, TRUE, MAKELPARAM(24, 120));
        SendMessageW(font, TBM_SETPOS, TRUE,
            static_cast<LPARAM>(std::round(app->style.font_size)));

        add_label(hwnd, L"Stroke", x, 176);
        const auto stroke_value_text = int_text(app->style.stroke_width) + L" px";
        app->stroke_value = make_control(hwnd, L"STATIC", stroke_value_text.c_str(),
            SS_RIGHT, 190, 176, 70, 22, id_stroke_value);
        const auto stroke = make_control(hwnd, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS,
            x, 200, 246, 34, id_stroke_width);
        SendMessageW(stroke, TBM_SETRANGE, TRUE, MAKELPARAM(0, 12));
        SendMessageW(stroke, TBM_SETPOS, TRUE,
            static_cast<LPARAM>(std::round(app->style.stroke_width)));

        add_label(hwnd, L"Spacing", x, 244);
        const auto spacing_value_text = int_text(app->style.letter_spacing) + L" px";
        app->spacing_value = make_control(hwnd, L"STATIC", spacing_value_text.c_str(),
            SS_RIGHT, 190, 244, 70, 22, id_spacing_value);
        const auto spacing = make_control(hwnd, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS,
            x, 268, 246, 34, id_letter_spacing);
        SendMessageW(spacing, TBM_SETRANGE, TRUE, MAKELPARAM(0, 35));
        SendMessageW(spacing, TBM_SETPOS, TRUE,
            static_cast<LPARAM>(std::round(app->style.letter_spacing + 5.0F)));

        make_control(hwnd, L"BUTTON", L"Fade", BS_AUTO3STATE,
            x, 316, 150, 24, id_fade);
        SendDlgItemMessageW(hwnd, id_fade, BM_SETCHECK,
            app->config.fade_mode == FadeMode::disabled ? BST_UNCHECKED
                : app->config.fade_mode == FadeMode::every_line
                    ? BST_INDETERMINATE : BST_CHECKED,
            0);
        make_control(hwnd, L"BUTTON", L"Indicator", BS_AUTOCHECKBOX,
            x, 346, 125, 24, id_indicator);
        SendDlgItemMessageW(hwnd, id_indicator, BM_SETCHECK,
            app->config.indicator.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        make_control(hwnd, L"BUTTON", L"Ruby isolate", BS_AUTOCHECKBOX,
            x + 130, 346, 125, 24, id_ruby_isolate);
        SendDlgItemMessageW(hwnd, id_ruby_isolate, BM_SETCHECK,
            app->style.ruby_isolate ? BST_CHECKED : BST_UNCHECKED, 0);

        make_control(hwnd, L"BUTTON", L"Role colors", BS_AUTOCHECKBOX,
            x, 376, 125, 24, id_role_colors);
        SendDlgItemMessageW(hwnd, id_role_colors, BM_SETCHECK,
            app->style.role_colors ? BST_CHECKED : BST_UNCHECKED, 0);
        make_control(hwnd, L"BUTTON", L"Role labels", BS_AUTOCHECKBOX,
            x + 130, 376, 125, 24, id_role_labels);
        SendDlgItemMessageW(hwnd, id_role_labels, BM_SETCHECK,
            app->style.show_role_labels ? BST_CHECKED : BST_UNCHECKED, 0);
        add_label(hwnd, L"Role", x, 408, 100);
        app->role_combo = make_control(hwnd, WC_COMBOBOXW, L"",
            CBS_DROPDOWNLIST | WS_VSCROLL, x + 104, 404, 105, 200, id_role_combo);
        make_control(hwnd, L"BUTTON", L"Img", BS_PUSHBUTTON,
            x + 214, 404, 40, 26, id_role_image_label);
        make_control(hwnd, L"BUTTON", L"Role before", BS_PUSHBUTTON,
            x, 438, 120, 30, id_role_before_color);
        make_control(hwnd, L"BUTTON", L"Role after", BS_PUSHBUTTON,
            x + 126, 438, 128, 30, id_role_after_color);

        make_control(hwnd, L"BUTTON", L"Before color", BS_PUSHBUTTON,
            x, 478, 246, 30, id_before_color);
        make_control(hwnd, L"BUTTON", L"After color", BS_PUSHBUTTON,
            x, 516, 246, 30, id_after_color);
        make_control(hwnd, L"BUTTON", L"BG/Video", BS_PUSHBUTTON,
            x, 554, 120, 30, id_load_background);
        make_control(hwnd, L"BUTTON", L"BG color", BS_PUSHBUTTON,
            x + 126, 554, 120, 30, id_background_color);

        add_label(hwnd, L"Font", x, 602, 35);
        const auto font_family = make_control(hwnd, WC_COMBOBOXW, L"",
            CBS_DROPDOWNLIST | WS_VSCROLL, x + 38, 596, 216, 160,
            id_font_family_combo);
        for (const auto* family : {L"MotoyaLMaru W3 Mono", L"Microsoft YaHei",
                L"Yu Gothic UI", L"Noto Sans CJK JP", L"Arial"}) {
            SendMessageW(font_family, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(family));
        }
        SendMessageW(font_family, CB_SETCURSEL, 0, 0);
        add_label(hwnd, L"Ruby size", x, 652, 64);
        const auto ruby_size_text = int_text(app->style.ruby_size);
        make_control(hwnd, L"EDIT", ruby_size_text.c_str(),
            WS_BORDER | ES_NUMBER | ES_CENTER,
            x + 68, 648, 50, 25, id_ruby_size_edit);
        add_label(hwnd, L"Ruby up", x + 128, 652, 52);
        const auto ruby_offset_text = int_text(app->style.ruby_above_offset);
        make_control(hwnd, L"EDIT", ruby_offset_text.c_str(),
            WS_BORDER | ES_NUMBER | ES_CENTER,
            x + 182, 648, 50, 25, id_ruby_offset_edit);
        add_label(hwnd, L"L1 X/Y", x, 684, 55);
        const auto line1_x_text = int_text(app->style.line1_x);
        make_control(hwnd, L"EDIT", line1_x_text.c_str(),
            WS_BORDER | ES_NUMBER | ES_CENTER,
            x + 58, 680, 44, 25, id_line1_x_edit);
        const auto line1_y_text = int_text(app->style.line1_y);
        make_control(hwnd, L"EDIT", line1_y_text.c_str(),
            WS_BORDER | ES_NUMBER | ES_CENTER,
            x + 106, 680, 44, 25, id_line1_y_edit);
        add_label(hwnd, L"L2 X/Y", x + 156, 684, 56);
        const auto line2_right_text = int_text(app->style.line2_right);
        make_control(hwnd, L"EDIT", line2_right_text.c_str(),
            WS_BORDER | ES_NUMBER | ES_CENTER,
            x + 212, 680, 42, 25, id_line2_right_edit);
        const auto line2_y_text = int_text(app->style.line2_y);
        make_control(hwnd, L"EDIT", line2_y_text.c_str(),
            WS_BORDER | ES_NUMBER | ES_CENTER,
            x + 212, 708, 42, 25, id_line2_y_edit);

        app->timeline = make_control(hwnd, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS,
            290, 640, 800, 30, id_timeline);
        SendMessageW(app->timeline, TBM_SETRANGE, TRUE, MAKELPARAM(0, 15000));
        app->status = make_control(hwnd, L"STATIC", L"", SS_LEFT,
            290, 674, 800, 24, id_status);

        DragAcceptFiles(hwnd, TRUE);
        SetTimer(hwnd, kFrameTimer, kFrameTimerMs, nullptr);
        app->load_builtin();
        refresh_role_combo(*app);
        update_status(*app);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        layout_controls(*app, rect.right, rect.bottom);
        return 0;
    }
    case WM_SIZE:
        if (app) layout_controls(*app, LOWORD(lparam), HIWORD(lparam));
        return 0;
    case kVideoEventMessage:
        if (app) {
            app->update_duration();
            app->current_time = app->video.current_time();
            SendMessageW(app->timeline, TBM_SETPOS, TRUE,
                static_cast<LPARAM>(app->current_time * 1000.0));
            InvalidateRect(app->preview, nullptr, FALSE);
            update_status(*app);
        }
        return 0;
    case WM_DROPFILES: {
        wchar_t path[32768]{};
        DragQueryFileW(reinterpret_cast<HDROP>(wparam), 0, path, std::size(path));
        DragFinish(reinterpret_cast<HDROP>(wparam));
        const auto extension = std::filesystem::path(path).extension().wstring();
        if (is_lyric_file(extension)) {
            app->lyric_path = path;
            rebuild_timeline(*app);
        } else if (is_image_file(extension) || is_video_file(extension)) {
            load_background_media(hwnd, *app, path);
        } else if (app->audio.open(path)) {
            app->audio_path = path;
            app->update_duration();
        }
        update_status(*app);
        return 0;
    }
    case WM_COMMAND:
        if (!app) break;
        if (LOWORD(wparam) == id_font_family_combo
                && HIWORD(wparam) == CBN_SELCHANGE) {
            const auto combo = reinterpret_cast<HWND>(lparam);
            const auto selection = static_cast<int>(SendMessageW(
                combo, CB_GETCURSEL, 0, 0));
            if (selection >= 0) {
                const auto length = static_cast<int>(SendMessageW(
                    combo, CB_GETLBTEXTLEN, selection, 0));
                std::wstring family(static_cast<std::size_t>(std::max(0, length)), L'\0');
                if (length > 0) SendMessageW(combo, CB_GETLBTEXT, selection,
                    reinterpret_cast<LPARAM>(family.data()));
                g_renderer.set_font_family(std::move(family));
                InvalidateRect(app->preview, nullptr, FALSE);
            }
            return 0;
        }
        if (HIWORD(wparam) == EN_CHANGE) {
            switch (LOWORD(wparam)) {
            case id_ruby_size_edit:
                app->style.ruby_size = static_cast<float>(std::clamp(
                    edit_integer(hwnd, id_ruby_size_edit, 26), 8, 96));
                break;
            case id_ruby_offset_edit:
                app->style.ruby_above_offset = static_cast<float>(std::clamp(
                    edit_integer(hwnd, id_ruby_offset_edit, 4), 0, 100));
                break;
            case id_line1_x_edit:
                app->style.line1_x = static_cast<float>(std::clamp(
                    edit_integer(hwnd, id_line1_x_edit, 128), 0, 1280));
                break;
            case id_line1_y_edit:
                app->style.line1_y = static_cast<float>(std::clamp(
                    edit_integer(hwnd, id_line1_y_edit, 430), 0, 720));
                break;
            case id_line2_right_edit:
                app->style.line2_right = static_cast<float>(std::clamp(
                    edit_integer(hwnd, id_line2_right_edit, 128), 0, 1280));
                break;
            case id_line2_y_edit:
                app->style.line2_y = static_cast<float>(std::clamp(
                    edit_integer(hwnd, id_line2_y_edit, 566), 0, 720));
                break;
            default:
                return 0;
            }
            InvalidateRect(app->preview, nullptr, FALSE);
            return 0;
        }
        switch (LOWORD(wparam)) {
        case id_load_lyrics: {
            const auto path = choose_file(hwnd, kLyricFilter);
            if (!path.empty()) {
                app->lyric_path = path;
                rebuild_timeline(*app);
                app->current_time = 0.0;
            }
            break;
        }
        case id_load_audio: {
            const auto path = choose_file(hwnd, kAudioFilter);
            if (!path.empty()) {
                const auto extension = std::filesystem::path(path).extension().wstring();
                if (is_video_file(extension)) {
                    load_background_media(hwnd, *app, path);
                } else if (app->audio.open(path)) {
                    app->audio_path = path;
                    app->update_duration();
                } else {
                    MessageBoxW(hwnd, L"Windows MCI could not open this audio. Prefer WAV or MP3 for this demo.",
                        L"Audio load failed", MB_ICONWARNING);
                }
            }
            break;
        }
        case id_save_frame: {
            const auto path = choose_save_png(hwnd);
            if (!path.empty() && !save_current_frame(*app, path)) {
                MessageBoxW(hwnd, L"Could not render or save PNG frame.",
                    L"Kirakara Show", MB_ICONERROR);
            }
            break;
        }
        case id_play:
            if (app->video.loaded) {
                app->video.toggle();
            } else if (app->audio.loaded) {
                if (app->audio.playing) app->audio.pause(); else app->audio.play();
            } else {
                app->local_playing = !app->local_playing;
                app->local_tick = std::chrono::steady_clock::now();
            }
            break;
        case id_fade:
            switch (SendDlgItemMessageW(hwnd, id_fade, BM_GETCHECK, 0, 0)) {
            case BST_CHECKED:
                app->config.fade_mode = FadeMode::paragraph_edges;
                break;
            case BST_INDETERMINATE:
                app->config.fade_mode = FadeMode::every_line;
                break;
            default:
                app->config.fade_mode = FadeMode::disabled;
                break;
            }
            rebuild_timeline(*app);
            break;
        case id_indicator:
            app->config.indicator.enabled = SendDlgItemMessageW(hwnd, id_indicator, BM_GETCHECK, 0, 0)
                == BST_CHECKED;
            rebuild_timeline(*app);
            break;
        case id_ruby_isolate:
            app->style.ruby_isolate = SendDlgItemMessageW(hwnd,
                id_ruby_isolate, BM_GETCHECK, 0, 0) == BST_CHECKED;
            InvalidateRect(app->preview, nullptr, FALSE);
            break;
        case id_role_colors:
            app->style.role_colors = SendDlgItemMessageW(hwnd,
                id_role_colors, BM_GETCHECK, 0, 0) == BST_CHECKED;
            InvalidateRect(app->preview, nullptr, FALSE);
            break;
        case id_role_labels:
            app->style.show_role_labels = SendDlgItemMessageW(hwnd,
                id_role_labels, BM_GETCHECK, 0, 0) == BST_CHECKED;
            InvalidateRect(app->preview, nullptr, FALSE);
            break;
        case id_role_image_label: {
            const auto role = selected_role(*app);
            if (!role.empty()) {
                const auto path = choose_file(hwnd, kImageFilter);
                if (!path.empty()) {
                    app->style.character_profiles[role].image_path = path;
                    app->style.show_role_labels = true;
                    SendDlgItemMessageW(hwnd, id_role_labels,
                        BM_SETCHECK, BST_CHECKED, 0);
                    InvalidateRect(app->preview, nullptr, FALSE);
                }
            }
            break;
        }
        case id_role_before_color: {
            const auto role = selected_role(*app);
            if (!role.empty()) {
                auto paint = role_paint(role, app->style);
                if (choose_color(hwnd, paint.before)) {
                    auto& p = app->style.character_profiles[role];
                    p.color_before = paint.before;
                    p.color_after  = paint.after;
                    p.stroke_before = paint.stroke_before;
                    p.stroke_after  = paint.stroke_after;
                    p.stroke_width  = paint.stroke_width;
                    InvalidateRect(app->preview, nullptr, FALSE);
                }
            }
            break;
        }
        case id_role_after_color: {
            const auto role = selected_role(*app);
            if (!role.empty()) {
                auto paint = role_paint(role, app->style);
                if (choose_color(hwnd, paint.after)) {
                    auto& p = app->style.character_profiles[role];
                    p.color_before = paint.before;
                    p.color_after  = paint.after;
                    p.stroke_before = paint.stroke_before;
                    p.stroke_after  = paint.stroke_after;
                    p.stroke_width  = paint.stroke_width;
                    InvalidateRect(app->preview, nullptr, FALSE);
                }
            }
            break;
        }
        case id_load_background: {
            const auto path = choose_file(hwnd, kBackgroundFilter);
            if (!path.empty()) {
                load_background_media(hwnd, *app, path);
            }
            break;
        }
        case id_before_color:
            if (choose_color(hwnd, app->style.before)) InvalidateRect(app->preview, nullptr, FALSE);
            break;
        case id_after_color:
            if (choose_color(hwnd, app->style.after)) InvalidateRect(app->preview, nullptr, FALSE);
            break;
        case id_background_color:
            if (choose_color(hwnd, app->style.background)) InvalidateRect(app->preview, nullptr, FALSE);
            break;
        }
        update_status(*app);
        return 0;
    case WM_HSCROLL:
        if (!app) break;
        if (reinterpret_cast<HWND>(lparam) == app->timeline) {
            app->dragging_timeline = LOWORD(wparam) == TB_THUMBTRACK;
            app->current_time = static_cast<double>(SendMessageW(app->timeline, TBM_GETPOS, 0, 0)) / 1000.0;
            if (LOWORD(wparam) == TB_ENDTRACK || LOWORD(wparam) == TB_THUMBPOSITION
                    || LOWORD(wparam) == TB_LINEUP || LOWORD(wparam) == TB_LINEDOWN
                    || LOWORD(wparam) == TB_PAGEUP || LOWORD(wparam) == TB_PAGEDOWN) {
                if (app->video.loaded) {
                    app->video.seek(app->current_time);
                } else {
                    app->audio.seek(app->current_time);
                }
                app->dragging_timeline = false;
            }
        } else {
            const auto id = GetDlgCtrlID(reinterpret_cast<HWND>(lparam));
            const auto position = static_cast<int>(SendMessageW(
                reinterpret_cast<HWND>(lparam), TBM_GETPOS, 0, 0));
            if (id == id_font_size) app->style.font_size = static_cast<float>(position);
            if (id == id_stroke_width) app->style.stroke_width = static_cast<float>(position);
            if (id == id_letter_spacing) app->style.letter_spacing = static_cast<float>(position - 5);
            update_value_labels(*app);
        }
        InvalidateRect(app->preview, nullptr, FALSE);
        update_status(*app);
        return 0;
    case WM_TIMER: {
        if (!app) break;
        bool repaint = false;
        if (!app->dragging_timeline) {
            if (app->video.loaded) {
                const auto next_time = app->video.current_time();
                repaint = app->video.playing
                    || std::abs(next_time - app->current_time) > 0.0005;
                app->current_time = next_time;
            } else if (app->audio.loaded) {
                app->audio.refresh_mode();
                if (app->audio.playing) {
                    const auto next_time = app->audio.estimated_position();
                    repaint = std::abs(next_time - app->current_time) > 0.0005;
                    app->current_time = next_time;
                }
            } else if (app->local_playing) {
                const auto now = std::chrono::steady_clock::now();
                app->current_time += std::chrono::duration<double>(now - app->local_tick).count();
                app->local_tick = now;
                if (app->current_time >= app->duration) {
                    app->current_time = 0.0;
                }
                repaint = true;
            }
            SendMessageW(app->timeline, TBM_SETPOS, TRUE,
                static_cast<LPARAM>(app->current_time * 1000.0));
        } else {
            repaint = true;
        }
        if (repaint) {
            InvalidateRect(app->preview, nullptr, FALSE);
            update_status(*app);
        }
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kFrameTimer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if constexpr (kEnableMediaEngineVideo) MFStartup(MF_VERSION);
    timeBeginPeriod(1);
    if (!g_renderer.initialize()) {
        MessageBoxW(nullptr, L"Direct2D / DirectWrite initialization failed.", L"Kirakara Show", MB_ICONERROR);
        timeEndPeriod(1);
        if constexpr (kEnableMediaEngineVideo) MFShutdown();
        CoUninitialize();
        return 1;
    }

    WNDCLASSW preview_class{};
    preview_class.hInstance = instance;
    preview_class.lpfnWndProc = preview_proc;
    preview_class.lpszClassName = kPreviewClass;
    preview_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    preview_class.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassW(&preview_class);

    WNDCLASSW main_class{};
    main_class.hInstance = instance;
    main_class.lpfnWndProc = main_proc;
    main_class.lpszClassName = kMainClass;
    main_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    main_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    main_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&main_class);

    AppState app;
    app.instance = instance;
    auto preset = default_app_config();
    const auto preset_path = std::filesystem::current_path()
        / L"config" / L"kirakara-show-default.json";
    static_cast<void>(load_app_config(preset_path.wstring(), preset));
    app.config = preset.engine;
    app.style = preset.style;
    g_renderer.set_font_families(preset.font_families);
    const auto window = CreateWindowExW(0, kMainClass, L"Kirakara Show - C++ realtime preview",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        1320, 820, nullptr, nullptr, instance, &app);
    if (!window) {
        g_renderer.shutdown();
        timeEndPeriod(1);
        if constexpr (kEnableMediaEngineVideo) MFShutdown();
        CoUninitialize();
        return 2;
    }
    ShowWindow(window, show);
    UpdateWindow(window);

    int argc{};
    std::wstring export_frame_path;
    bool exit_after_export = false;
    int export_result = 0;
    int benchmark_frames = 0;
    std::wstring benchmark_json_path;
    bool exit_after_benchmark = false;
    bool benchmark_gpu = false;
    double benchmark_rate = 0.0;
    auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring_view argument = argv[i];
            if (argument.starts_with(L"--role-image=")) {
                const auto value = argument.substr(13);
                const auto equals = value.find(L'=');
                if (equals != std::wstring_view::npos && equals > 0
                        && equals + 1 < value.size()) {
                    const auto role = wide_to_utf8(value.substr(0, equals));
                    app.style.character_profiles[role].image_path =
                        std::wstring{value.substr(equals + 1)};
                    app.style.show_role_labels = true;
                    SendDlgItemMessageW(window, id_role_labels,
                        BM_SETCHECK, BST_CHECKED, 0);
                }
                continue;
            }
            if (argument.starts_with(L"--role-label-name=")) {
                const auto value = argument.substr(18);
                const auto equals = value.find(L'=');
                if (equals != std::wstring_view::npos && equals > 0) {
                    const auto role = wide_to_utf8(value.substr(0, equals));
                    app.style.character_profiles[role].display_name =
                        wide_to_utf8(value.substr(equals + 1));
                    app.style.show_role_labels = true;
                    SendDlgItemMessageW(window, id_role_labels,
                        BM_SETCHECK, BST_CHECKED, 0);
                }
                continue;
            }
            if (argument.starts_with(L"--role-label-scale=")) {
                const auto value = argument.substr(19);
                const auto equals = value.find(L'=');
                if (equals != std::wstring_view::npos && equals > 0) {
                    const auto role = wide_to_utf8(value.substr(0, equals));
                    app.style.character_profiles[role].label_scale =
                        std::clamp(static_cast<float>(_wtof(
                            std::wstring{value.substr(equals + 1)}.c_str())),
                            10.0F, 400.0F);
                }
                continue;
            }
            if (argument.starts_with(L"--role-label-offset-y=")) {
                const auto value = argument.substr(22);
                const auto equals = value.find(L'=');
                if (equals != std::wstring_view::npos && equals > 0) {
                    const auto role = wide_to_utf8(value.substr(0, equals));
                    app.style.character_profiles[role].label_offset_y =
                        static_cast<float>(_wtof(
                            std::wstring{value.substr(equals + 1)}.c_str()));
                }
                continue;
            }
            if (argument.starts_with(L"--role-label-margins=")) {
                const auto value = argument.substr(21);
                const auto equals = value.find(L'=');
                const auto comma = value.find(L',', equals == std::wstring_view::npos
                    ? 0 : equals + 1);
                if (equals != std::wstring_view::npos && comma != std::wstring_view::npos
                        && equals > 0) {
                    const auto role = wide_to_utf8(value.substr(0, equals));
                    auto& profile = app.style.character_profiles[role];
                    profile.label_margin_left = static_cast<float>(_wtof(
                        std::wstring{value.substr(equals + 1,
                            comma - equals - 1)}.c_str()));
                    profile.label_margin_right = static_cast<float>(_wtof(
                        std::wstring{value.substr(comma + 1)}.c_str()));
                }
                continue;
            }
            if (argument.starts_with(L"--font-bold=")) {
                app.style.font_bold = argument.substr(12) != L"0"
                    && argument.substr(12) != L"false";
                continue;
            }
            if (argument.starts_with(L"--ruby-bold=")) {
                app.style.ruby_bold = argument.substr(12) != L"0"
                    && argument.substr(12) != L"false";
                continue;
            }
            if (argument.starts_with(L"--ruby2-bold=")) {
                app.style.ruby2_bold = argument.substr(13) != L"0"
                    && argument.substr(13) != L"false";
                continue;
            }
            if (argument.starts_with(L"--time=")) {
                app.current_time = std::clamp(_wtof(argv[i] + 7), 0.0, app.duration);
                SendMessageW(app.timeline, TBM_SETPOS, TRUE,
                    static_cast<LPARAM>(app.current_time * 1000.0));
                continue;
            }
            if (argument.starts_with(L"--export-frame=")) {
                export_frame_path = argument.substr(15);
                continue;
            }
            if (argument == L"--exit-after-export") {
                exit_after_export = true;
                continue;
            }
            if (argument.starts_with(L"--benchmark-frames=")) {
                benchmark_frames = std::clamp(_wtoi(argv[i] + 19), 1, 100000);
                continue;
            }
            if (argument.starts_with(L"--benchmark-json=")) {
                benchmark_json_path = argument.substr(17);
                continue;
            }
            if (argument.starts_with(L"--benchmark-rate=")) {
                benchmark_rate = std::clamp(_wtof(argv[i] + 17), 0.0, 1000.0);
                continue;
            }
            if (argument == L"--exit-after-benchmark") {
                exit_after_benchmark = true;
                continue;
            }
            if (argument == L"--benchmark-gpu") {
                benchmark_gpu = true;
                continue;
            }
            const auto extension = std::filesystem::path(argv[i]).extension().wstring();
            if (is_lyric_file(extension)) {
                app.lyric_path = argv[i];
                rebuild_timeline(app);
            } else if (is_image_file(extension) || is_video_file(extension)) {
                load_background_media(window, app, argv[i]);
            } else if (app.audio.open(argv[i])) {
                app.audio_path = argv[i];
                app.update_duration();
            }
        }
        LocalFree(argv);
    }
    update_status(app);
    if (!export_frame_path.empty()) {
        if (!save_current_frame(app, export_frame_path)) {
            export_result = 3;
            MessageBoxW(window, L"Could not render or save the requested PNG frame.",
                L"Kirakara Show", MB_ICONERROR);
        }
        if (exit_after_export) {
            DestroyWindow(window);
        }
    }
    if (benchmark_frames > 0) {
        if (!benchmark_renderer(app, benchmark_frames, benchmark_json_path,
                benchmark_gpu, benchmark_rate)) {
            export_result = 4;
            MessageBoxW(window, L"1920x1080 render benchmark failed.",
                L"Kirakara Show", MB_ICONERROR);
        }
        if (exit_after_benchmark && IsWindow(window)) {
            DestroyWindow(window);
        }
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    g_renderer.shutdown();
    timeEndPeriod(1);
    if constexpr (kEnableMediaEngineVideo) MFShutdown();
    CoUninitialize();
    return export_result != 0 ? export_result : static_cast<int>(message.wParam);
}
