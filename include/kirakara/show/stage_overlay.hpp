#pragma once

#include "kirakara/show/stage_frame.hpp"
#include "kirakara/show/types.hpp"

#include <cstdint>
#include <string>

namespace kirakara::show {

// Assets are identified independently from their local cache path. A renderer
// only uploads a replacement when cache_key or content_revision changes.
struct StageOverlayAsset {
    std::wstring cache_key;
    std::wstring local_path;
    std::uint64_t content_revision{};

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool same_content_as(
        const StageOverlayAsset& other) const noexcept;
};

struct StageQrOverlayState {
    bool visible{};
    std::wstring payload;
    StageOverlayAsset decoration;
};

struct StageAnnouncementOverlayState {
    bool visible{};
    std::wstring text;
    StageOverlayAsset decoration;
};

struct StageOverlayState {
    std::uint64_t revision{};
    StageQrOverlayState qr;
    StageAnnouncementOverlayState announcement;

    [[nodiscard]] bool valid() const noexcept;
};

struct StageOverlayRenderContext {
    StageOutputProfile profile;
    Seconds project_time{};
    void* native_render_target{};
};

// The concrete GPU cache arrives with the final commissioned assets. Keeping
// it behind this interface prevents App paths and image decoding policy from
// leaking into the Stage compositor.
class StageOverlayAssetCache {
public:
    virtual ~StageOverlayAssetCache() = default;
    [[nodiscard]] virtual bool prepare(
        const StageOverlayAsset& asset) = 0;
    virtual void discard(const std::wstring& cache_key) = 0;
};

// Unified Stage will call one implementation for controller preview, physical
// display and Cast. Phase 6 intentionally defines the boundary without
// drawing temporary QR or announcement artwork.
class StageOverlayRenderer {
public:
    virtual ~StageOverlayRenderer() = default;
    [[nodiscard]] virtual bool update(
        const StageOverlayState& state,
        StageOverlayAssetCache& assets) = 0;
    [[nodiscard]] virtual bool render(
        const StageOverlayRenderContext& context) = 0;
};

} // namespace kirakara::show
