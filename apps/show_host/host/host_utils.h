#pragma once

#include <windows.h>
#include <cstdint>
#include <fstream>
#include <string>

// ── Window class names ───────────────────────────────────────────

constexpr wchar_t kStageClass[] = L"KirakaraShowHostStage";
constexpr wchar_t kVideoBlankClass[] = L"KirakaraShowHostVideoBlank";
constexpr wchar_t kMediaClockClass[] = L"KirakaraShowHostMediaClock";

// ── Window messages ──────────────────────────────────────────────

constexpr UINT kVideoEventMessage = WM_APP + 21;
constexpr UINT kHostLoadMessage = WM_APP + 101;
constexpr UINT kHostPlayMessage = WM_APP + 102;
constexpr UINT kHostPauseMessage = WM_APP + 103;
constexpr UINT kHostStopMessage = WM_APP + 104;
constexpr UINT kHostSeekMessage = WM_APP + 105;
constexpr UINT kHostAudioTrackMessage = WM_APP + 106;
constexpr UINT kHostStageVisibleMessage = WM_APP + 107;
constexpr UINT kHostVolumeMessage = WM_APP + 111;
constexpr UINT kHostStageWindowRectMessage = WM_APP + 112;
constexpr UINT kHostKeyMessage = WM_APP + 113;
constexpr UINT kHostAudioClockOffsetMessage = WM_APP + 114;
constexpr UINT kHostStartCastStreamMessage = WM_APP + 115;
constexpr UINT kHostStopCastStreamMessage = WM_APP + 116;
constexpr UINT kHostPrepareNextMessage = WM_APP + 117;
constexpr UINT kHostStageDeviceMessage = WM_APP + 118;
constexpr UINT kHostStageOverlayStateMessage = WM_APP + 119;
constexpr UINT kCastMediaEndedMessage = WM_APP + 120;
constexpr UINT kCastFatalErrorMessage = WM_APP + 121;

// ── Dimensions ───────────────────────────────────────────────────

constexpr std::uint32_t kInitialWidth = 1280;
constexpr std::uint32_t kInitialHeight = 720;

// ── Audio / math constants ───────────────────────────────────────

constexpr double kKeyFadeSeconds = 0.032;
constexpr double kHalfPi = 1.57079632679489661923;

// ── Debug log ────────────────────────────────────────────────────

extern std::ofstream g_debug_log;

void log_line(const char* text);
void log_hresult(const char* label, HRESULT result);

// Available since Windows 10. Resolve dynamically so the host DLL retains its
// existing load compatibility on older systems and MinGW SDK configurations.
[[nodiscard]] DPI_AWARENESS_CONTEXT set_thread_dpi_awareness_context_if_available(
    DPI_AWARENESS_CONTEXT awareness) noexcept;

// ── COM helpers ──────────────────────────────────────────────────

template <typename T>
void release_com(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

// ── Path / URL helpers ───────────────────────────────────────────

std::wstring file_url_from_path(const std::wstring& path);
std::string read_file(const std::wstring& path);

// ── Formatting ───────────────────────────────────────────────────

std::wstring hresult_text(HRESULT result);
std::wstring format_time(double seconds);
