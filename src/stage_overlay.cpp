#include "kirakara/show/stage_overlay.hpp"

namespace kirakara::show {

bool StageOverlayAsset::valid() const noexcept {
    return cache_key.empty() == local_path.empty();
}

bool StageOverlayAsset::same_content_as(
        const StageOverlayAsset& other) const noexcept {
    return cache_key == other.cache_key
        && content_revision == other.content_revision;
}

bool StageOverlayState::valid() const noexcept {
    return (!qr.visible || !qr.payload.empty())
        && (!announcement.visible || !announcement.text.empty())
        && qr.decoration.valid()
        && announcement.decoration.valid();
}

} // namespace kirakara::show
