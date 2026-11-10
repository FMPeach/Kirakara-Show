#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

inline constexpr std::uint32_t kHostHudAbiVersion = 1;
inline constexpr std::size_t kHostHudMaxQrBytes = 4U * 1024U * 1024U;
inline constexpr std::uint32_t kHostHudMaxQrPixels = 2048;
inline constexpr std::uint32_t kHostHudMaxQrModules = 185;
inline constexpr std::size_t kHostHudMaxQueueItems = 4;
inline constexpr std::size_t kHostHudMaxTextUnits = 512;

struct HostHudOptions {
    bool qr_enabled{};
    bool queue_enabled{};
    bool announcement_enabled{};
    bool operator==(const HostHudOptions&) const = default;
};

// An App-produced PNG, including its quiet zone. modules describes the whole
// image so the renderer can check its density without encoding a QR.
struct HostHudQrImage {
    std::uint64_t revision{};
    // Stamped by Host on receipt; a network event requires App to confirm
    // its current shared image before the card is shown again.
    std::uint64_t network_revision{};
    std::uint32_t pixel_size{};
    std::uint32_t modules{};
    std::vector<std::uint8_t> png;

    [[nodiscard]] bool valid() const noexcept {
        return revision != 0 && pixel_size > 0
            && pixel_size <= kHostHudMaxQrPixels && modules > 0
            && modules <= kHostHudMaxQrModules && pixel_size % modules == 0
            && !png.empty() && png.size() <= kHostHudMaxQrBytes;
    }
};

struct HostHudQueueItem {
    std::wstring title;
    std::wstring artist;
    bool operator==(const HostHudQueueItem&) const = default;
};

struct HostHudState {
    std::uint64_t revision{};
    HostHudOptions options;
    std::shared_ptr<const HostHudQrImage> qr;
    std::wstring network_name;
    bool network_wifi{};
    std::vector<HostHudQueueItem> queue;
    HostHudQueueItem current;

    [[nodiscard]] bool same_content(const HostHudState& other) const noexcept {
        return options == other.options && network_name == other.network_name
            && network_wifi == other.network_wifi && queue == other.queue && current == other.current;
    }

    [[nodiscard]] bool valid() const noexcept {
        if ((qr && !qr->valid()) || network_name.size() > kHostHudMaxTextUnits
                || current.title.size() > kHostHudMaxTextUnits || current.artist.size() > kHostHudMaxTextUnits
                || queue.size() > kHostHudMaxQueueItems) return false;
        for (const auto& item : queue) {
            if (item.title.size() > kHostHudMaxTextUnits
                    || item.artist.size() > kHostHudMaxTextUnits) return false;
        }
        return true;
    }
};
