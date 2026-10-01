#include "directshow_backend.h"

#include "../host/host_utils.h"

#include <algorithm>
#include <cmath>

#include <dshow.h>
#include <windows.h>

#pragma comment(lib, "quartz")
#pragma comment(lib, "strmiids")

DirectShowAudioBackend::~DirectShowAudioBackend() { close(); }

bool DirectShowAudioBackend::open(const std::wstring& path) {
    close();
    if (path.empty()) return false;
    HRESULT result = CoCreateInstance(CLSID_FilterGraph, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph_));
    if (SUCCEEDED(result)) {
        result = graph_->RenderFile(path.c_str(), nullptr);
    }
    if (SUCCEEDED(result)) {
        result = graph_->QueryInterface(IID_PPV_ARGS(&control_));
    }
    if (graph_) {
        static_cast<void>(
            graph_->QueryInterface(IID_PPV_ARGS(&seeking_)));
        static_cast<void>(graph_->QueryInterface(IID_IBasicAudio,
            reinterpret_cast<void**>(&basic_audio_)));
    }
    if (FAILED(result) || !control_) {
        close();
        return false;
    }
    if (seeking_) {
        static_cast<void>(seeking_->SetTimeFormat(&TIME_FORMAT_MEDIA_TIME));
    }
    opened_ = true;
    set_volume(volume_percent_);
    set_key_semitones(key_semitones_);
    return true;
}

bool DirectShowAudioBackend::load_second_track(const std::wstring&) {
    return false;
}

void DirectShowAudioBackend::set_active_track(int) {}

void DirectShowAudioBackend::close() {
    if (control_) {
        static_cast<void>(control_->Stop());
    }
    release_com(basic_audio_);
    release_com(seeking_);
    release_com(control_);
    release_com(graph_);
    opened_ = false;
    playing_ = false;
}

bool DirectShowAudioBackend::play() {
    if (!opened_ || !control_) return false;
    const auto result = control_->Run();
    if (SUCCEEDED(result)) playing_ = true;
    return SUCCEEDED(result);
}

bool DirectShowAudioBackend::pause() {
    if (!opened_ || !control_) return false;
    const auto result = control_->Pause();
    if (SUCCEEDED(result)) playing_ = false;
    return SUCCEEDED(result);
}

bool DirectShowAudioBackend::stop() {
    if (!opened_ || !control_) return false;
    const bool stopped = SUCCEEDED(control_->Stop());
    const bool seeked = seek(0.0);
    playing_ = false;
    return stopped && seeked;
}

bool DirectShowAudioBackend::seek(double seconds) {
    if (!opened_ || !seeking_) return false;
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    auto position = static_cast<LONGLONG>(seconds * 10000000.0);
    return SUCCEEDED(seeking_->SetPositions(&position,
        AM_SEEKING_AbsolutePositioning, nullptr,
        AM_SEEKING_NoPositioning));
}

bool DirectShowAudioBackend::set_volume(int percent) {
    volume_percent_ = std::clamp(percent, 0, 100);
    return apply_output_volume();
}

bool DirectShowAudioBackend::set_local_output_enabled(bool enabled) {
    local_output_enabled_ = enabled;
    return apply_output_volume();
}

bool DirectShowAudioBackend::apply_output_volume() {
    if (!opened_ || !basic_audio_) return true;
    long directshow_volume = -10000;
    if (local_output_enabled_ && volume_percent_ > 0) {
        const auto ratio = static_cast<double>(volume_percent_) / 100.0;
        directshow_volume = std::clamp<long>(
            static_cast<long>(std::lround(2000.0 * std::log10(ratio))),
            -10000, 0);
    }
    return SUCCEEDED(basic_audio_->put_Volume(directshow_volume));
}

bool DirectShowAudioBackend::set_key_semitones(int semitones) {
    key_semitones_ = std::clamp(semitones, -6, 6);
    return key_semitones_ == 0;
}

double DirectShowAudioBackend::position() const {
    if (!opened_ || !seeking_) return 0.0;
    LONGLONG value{};
    if (FAILED(seeking_->GetCurrentPosition(&value))) return 0.0;
    return static_cast<double>(value) / 10000000.0;
}

double DirectShowAudioBackend::duration() const {
    if (!opened_ || !seeking_) return 0.0;
    LONGLONG value{};
    if (FAILED(seeking_->GetDuration(&value))) return 0.0;
    return static_cast<double>(value) / 10000000.0;
}

bool DirectShowAudioBackend::is_open() const noexcept { return opened_; }
