#pragma once

#include <mfobjects.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Observes byte I/O only. SourceReader ReadSample also includes decode and GPU
// work, so its latency cannot be used as evidence of a network buffer wait.
class MfSourceByteStreamReads {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;

    std::uint64_t begin(Time now = Clock::now(),
            std::function<bool()> bytes_available = {}) {
        std::lock_guard lock(mutex_);
        const auto id = ++next_id_;
        pending_.emplace(id, Pending{now, std::move(bytes_available)});
        return id;
    }

    void reset() {
        std::lock_guard lock(mutex_);
        pending_.clear();
        failed_ = false;
        premature_eof_ = false;
    }

    void finish(std::uint64_t id, bool failed, bool premature_eof,
            bool /* received_bytes */) {
        std::lock_guard lock(mutex_);
        if (pending_.erase(id) == 0) return;
        if (failed || premature_eof) {
            failed_ = true;
            premature_eof_ = premature_eof;
        } else {
            failed_ = false;
            premature_eof_ = false;
        }
    }

    void cancel(std::uint64_t id) {
        std::lock_guard lock(mutex_);
        pending_.erase(id);
    }

    [[nodiscard]] bool buffering(Time now = Clock::now()) const {
        std::vector<std::pair<std::uint64_t, std::function<bool()>>> checks;
        {
            std::lock_guard lock(mutex_);
            if (failed_) return true;
            for (const auto& [id, read] : pending_) {
                if (now - read.started >= std::chrono::milliseconds(100)) {
                    checks.emplace_back(id, read.bytes_available);
                }
            }
        }
        // Check local cache metadata outside the completion lock. Never call
        // back into an MF byte stream from the program-clock thread.
        for (const auto& [id, available] : checks) {
            if (available && available()) continue;
            std::lock_guard lock(mutex_);
            if (pending_.contains(id)) return true;
        }
        return false;
    }

    [[nodiscard]] bool premature_eof() const {
        std::lock_guard lock(mutex_);
        return premature_eof_;
    }

private:
    mutable std::mutex mutex_;
    struct Pending {
        Time started;
        std::function<bool()> bytes_available;
    };
    std::uint64_t next_id_{};
    std::unordered_map<std::uint64_t, Pending> pending_;
    bool failed_{};
    bool premature_eof_{};
};

// Optional App loopback metadata; never interpreted for remote media URLs.
// A complete file cannot be network starvation. A partial prefix is not enough:
// MF's HTTP provider can wait on read-ahead beyond the requested byte range.
std::function<bool()> make_local_cache_completion_probe(
    const std::wstring& source_url, QWORD source_length);

// Preserves the system HTTP byte stream, including its range handling and
// cache, while observing reads below the decoder. It does not enable MF's
// additional network prebuffer or change App/Show's public ABI.
HRESULT create_observed_source_byte_stream(IMFByteStream* source,
    std::shared_ptr<MfSourceByteStreamReads> reads,
    IMFByteStream** result, const std::wstring& source_url = {});
