#include "host_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include <shlwapi.h>

#pragma comment(lib, "shlwapi")

std::ofstream g_debug_log;

DPI_AWARENESS_CONTEXT set_thread_dpi_awareness_context_if_available(
        DPI_AWARENESS_CONTEXT awareness) noexcept {
    using SetThreadDpiAwarenessContextFn = DPI_AWARENESS_CONTEXT(WINAPI*)(
        DPI_AWARENESS_CONTEXT);
    static const auto function = reinterpret_cast<
        SetThreadDpiAwarenessContextFn>(GetProcAddress(
            GetModuleHandleW(L"user32.dll"),
            "SetThreadDpiAwarenessContext"));
    return function ? function(awareness) : nullptr;
}

void log_line(const char* text) {
    // Debug log file output is disabled for the release build. Every probe
    // trace and diagnostic marker has been removed; leaving this sink enabled
    // would still produce a show_host_debug.log from ordinary log_line /
    // log_hresult calls. Re-enable by restoring the ofstream write below if
    // diagnostic logging is ever needed again.
    (void)text;
    (void)g_debug_log;
}

void log_hresult(const char* label, HRESULT result) {
    char buffer[128]{};
    std::snprintf(buffer, std::size(buffer), "%s 0x%08lx", label,
        static_cast<unsigned long>(result));
    log_line(buffer);
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

std::string read_file(const std::wstring& path) {
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    return {std::istreambuf_iterator<char>{stream},
        std::istreambuf_iterator<char>{}};
}

std::wstring hresult_text(HRESULT result) {
    std::wstringstream stream;
    stream << L"0x" << std::hex << static_cast<unsigned long>(result);
    return stream.str();
}

std::wstring format_time(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    const auto minutes = static_cast<int>(seconds) / 60;
    const auto remainder = seconds - minutes * 60;
    wchar_t buffer[64]{};
    swprintf(buffer, std::size(buffer), L"%02d:%05.2f", minutes, remainder);
    return buffer;
}
