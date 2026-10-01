#include "kirakara/show/win32/unified_stage_renderer.hpp"

#include "kirakara/show/win32/lyric_renderer.hpp"
#include "kirakara/show/win32/song_title_renderer.hpp"
#include "kirakara/show/win32/stage_compositor.hpp"
#include "kirakara/show/win32/stage_frame_resource.hpp"

#include <memory>
#include <utility>

#include <windows.h>

namespace kirakara::show::win32 {
namespace {

StageCanvas canvas_from_profile(const StageOutputProfile& profile) {
    return StageCanvas{
        profile.width,
        profile.height,
        profile.refresh_rate_num,
        profile.refresh_rate_den,
    };
}

bool valid_video_input(const UnifiedStageRenderRequest& request) {
    const bool has_texture = request.video_texture != nullptr;
    const bool has_size = request.video_width > 0 && request.video_height > 0;
    if (has_texture != has_size) return false;
    if (!request.video_identity.valid) return true;
    return has_texture
        && request.video_identity.width == request.video_width
        && request.video_identity.height == request.video_height;
}

bool same_color(const Color& left, const Color& right) noexcept {
    return left.r == right.r && left.g == right.g
        && left.b == right.b && left.a == right.a;
}

bool same_video_identity(const UnifiedStageVideoFrameIdentity& left,
        const UnifiedStageVideoFrameIdentity& right) noexcept {
    return left.valid && right.valid
        && left.generation == right.generation
        && left.frame_id == right.frame_id
        && left.width == right.width
        && left.height == right.height;
}

std::uint64_t qpc_now() noexcept {
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value)
        ? static_cast<std::uint64_t>(value.QuadPart) : 0;
}

class OptionalTiming final {
public:
    explicit OptionalTiming(std::uint64_t* destination) noexcept
        : destination_(destination),
          started_(destination ? qpc_now() : 0) {}

    ~OptionalTiming() {
        if (!destination_) return;
        const auto finished = qpc_now();
        *destination_ += finished >= started_ ? finished - started_ : 0;
    }

    OptionalTiming(const OptionalTiming&) = delete;
    OptionalTiming& operator=(const OptionalTiming&) = delete;

private:
    std::uint64_t* destination_{};
    std::uint64_t started_{};
};

} // namespace

struct UnifiedStageRenderer::Impl {
    struct TargetRenderers {
        void* target{};
        LyricRenderer lyrics;
        SongTitleRenderer title;

        bool initialize(void* next_target) {
            if (!next_target || !lyrics.initialize() || !title.initialize()
                    || !attach(next_target)) {
                shutdown();
                return false;
            }
            return true;
        }

        bool attach(void* next_target) {
            if (!next_target) return false;
            if (target == next_target) return true;
            if (!lyrics.attach_native_target(next_target)
                    || !title.attach_native_target(next_target)) {
                return false;
            }
            target = next_target;
            return true;
        }

        void shutdown() noexcept {
            title.shutdown();
            lyrics.shutdown();
            target = nullptr;
        }
    };

    StageOutputProfile profile;
    StageCompositor compositor;
    D3D11TextureSurface video_base_surface;
    StageFramePool pool;
    StageFrameHub hub;
    std::unique_ptr<TargetRenderers> target_renderers;
    bool is_configured{};
    std::uint64_t rendered{};
    std::uint64_t dropped{};
    UnifiedStageVideoFrameIdentity cached_video_identity;
    Color cached_video_idle_color{};
    bool video_base_valid{};

    void invalidate_video_base() noexcept {
        cached_video_identity = {};
        cached_video_idle_color = {};
        video_base_valid = false;
    }

    void release_target_renderers() noexcept {
        if (target_renderers) target_renderers->shutdown();
        target_renderers.reset();
    }

    TargetRenderers* renderers_for(void* target) {
        if (!target) return nullptr;
        if (target_renderers) {
            return target_renderers->attach(target)
                ? target_renderers.get() : nullptr;
        }
        auto renderers = std::make_unique<TargetRenderers>();
        if (!renderers->initialize(target)) return nullptr;
        target_renderers = std::move(renderers);
        return target_renderers.get();
    }

    bool configure(const StageOutputProfile& next_profile,
            std::size_t capacity,
            void* native_d3d11_device) {
        if (!next_profile.valid()
                || next_profile.pixel_format != StagePixelFormat::bgra8
                || capacity == 0) {
            return false;
        }
        is_configured = false;
        release_target_renderers();
        video_base_surface = D3D11TextureSurface{};
        invalidate_video_base();
        if (!compositor.configure(
                canvas_from_profile(next_profile), native_d3d11_device)) {
            return false;
        }
        // Unified Stage owns both interoperability boundaries below. Record
        // all D2D batches first instead of flushing after clear and video
        // separately; this preserves ordering while avoiding redundant GPU
        // submissions within one output frame.
        compositor.set_defer_d2d_interop_flush(true);
        auto* device = compositor.current_frame().d3d_device;
        if (!device) return false;
        if (!video_base_surface.resize_on_device(
                device, next_profile.width, next_profile.height)) {
            return false;
        }
        const auto* resource_domain = &compositor.overlay_surface();
        if (!pool.configure(next_profile,
                [resource_domain](const StageOutputProfile& slot_profile,
                        std::size_t) {
                    return D3D11StageFrameResource::create(
                        slot_profile, *resource_domain);
                }, capacity)) {
            return false;
        }
        profile = next_profile;
        is_configured = true;
        return true;
    }

    UnifiedStageRenderResult render(
            const UnifiedStageRenderRequest& request) {
        if (!is_configured || request.timing.duration_100ns <= 0
                || !valid_video_input(request)) {
            return UnifiedStageRenderResult::invalid_request;
        }

        if (request.diagnostics) *request.diagnostics = {};
        const auto pool_started = request.diagnostics ? qpc_now() : 0;
        auto write = pool.try_acquire();
        if (request.diagnostics) {
            const auto pool_finished = qpc_now();
            request.diagnostics->pool_acquire_qpc =
                pool_finished >= pool_started
                ? pool_finished - pool_started : 0;
        }
        if (!write) {
            ++dropped;
            return UnifiedStageRenderResult::dropped_no_free_frame;
        }
        auto* resource = dynamic_cast<D3D11StageFrameResource*>(
            write.resource());
        if (!resource) return UnifiedStageRenderResult::render_failed;

        const auto source_width = request.video_texture
            ? request.video_width : 0;
        const auto source_height = request.video_texture
            ? request.video_height : 0;
        if (!request.video_texture) invalidate_video_base();

        const bool cacheable_video = request.video_texture
            && request.video_identity.valid;
        if (cacheable_video) {
            const bool refresh_video_base = !video_base_valid
                || !same_video_identity(cached_video_identity,
                    request.video_identity)
                || !same_color(cached_video_idle_color, request.idle_color);
            if (refresh_video_base) {
                {
                    OptionalTiming timing(request.diagnostics
                        ? &request.diagnostics->begin_clear_qpc : nullptr);
                    if (!compositor.begin_frame_on(video_base_surface,
                            source_width, source_height,
                            request.idle_color)) {
                        invalidate_video_base();
                        return UnifiedStageRenderResult::render_failed;
                    }
                }
                {
                    OptionalTiming timing(request.diagnostics
                        ? &request.diagnostics->video_composite_qpc
                        : nullptr);
                    if (!compositor.composite_video_texture(
                            request.video_texture,
                            request.video_width,
                            request.video_height)) {
                        invalidate_video_base();
                        return UnifiedStageRenderResult::render_failed;
                    }
                }
                // The cached base is about to cross from D2D rendering to a
                // D3D11 CopyResource. Flush once after clear + scale; an
                // immediate-context Flush is deliberately unnecessary on the
                // same device.
                {
                    OptionalTiming timing(request.diagnostics
                        ? &request.diagnostics->d2d_flush_qpc : nullptr);
                    video_base_surface.flush_d2d();
                    if (request.diagnostics) {
                        ++request.diagnostics->d2d_flush_count;
                    }
                }
                cached_video_identity = request.video_identity;
                cached_video_idle_color = request.idle_color;
                video_base_valid = true;
                if (request.diagnostics) {
                    request.diagnostics->video_cache_refreshed = true;
                }
            } else if (request.diagnostics) {
                request.diagnostics->video_cache_reused = true;
            }

            {
                OptionalTiming timing(request.diagnostics
                    ? &request.diagnostics->video_cache_copy_qpc : nullptr);
                if (!resource->surface().copy_from(video_base_surface)) {
                    invalidate_video_base();
                    return UnifiedStageRenderResult::render_failed;
                }
            }
            if (!compositor.prepare_frame_on(resource->surface(),
                    source_width, source_height)) {
                return UnifiedStageRenderResult::render_failed;
            }
        } else {
            {
                OptionalTiming timing(request.diagnostics
                    ? &request.diagnostics->begin_clear_qpc : nullptr);
                if (!compositor.begin_frame_on(resource->surface(),
                        source_width, source_height,
                        request.idle_color)) {
                    return UnifiedStageRenderResult::render_failed;
                }
            }
            if (request.video_texture) {
                OptionalTiming timing(request.diagnostics
                    ? &request.diagnostics->video_composite_qpc : nullptr);
                if (!compositor.composite_video_texture(
                        request.video_texture,
                        request.video_width,
                        request.video_height)) {
                    return UnifiedStageRenderResult::render_failed;
                }
            }
        }
        auto* target = resource->surface().native_render_target();
        if (!target) return UnifiedStageRenderResult::render_failed;
        auto* renderers = request.config
                && (request.draw_song_title || request.draw_lyrics)
            ? renderers_for(target) : nullptr;
        if (request.config && (request.draw_song_title || request.draw_lyrics)
                && !renderers) {
            return UnifiedStageRenderResult::render_failed;
        }
        if (request.config && request.draw_song_title) {
            OptionalTiming timing(request.diagnostics
                ? &request.diagnostics->title_render_qpc : nullptr);
            if (!renderers->title.render(request.config->song_title,
                        request.project_time,
                        request.config->engine.fade_duration)) {
                return UnifiedStageRenderResult::render_failed;
            }
        }
        if (request.lyrics && request.config && request.draw_lyrics) {
            OptionalTiming timing(request.diagnostics
                ? &request.diagnostics->lyric_render_qpc : nullptr);
            renderers->lyrics.set_font_families(request.config->font_families);
            if (!renderers->lyrics.render_overlay(*request.lyrics,
                        request.project_time, request.config->engine,
                        request.config->style)) {
                return UnifiedStageRenderResult::render_failed;
            }
        }
        {
            OptionalTiming timing(request.diagnostics
                ? &request.diagnostics->d2d_flush_qpc : nullptr);
            resource->surface().flush_d2d();
            if (request.diagnostics) {
                ++request.diagnostics->d2d_flush_count;
            }
        }
        if (request.flush_d3d_before_publish) {
            OptionalTiming timing(request.diagnostics
                ? &request.diagnostics->d3d_flush_qpc : nullptr);
            resource->surface().flush_d3d();
            if (request.diagnostics) {
                ++request.diagnostics->d3d_flush_count;
            }
        }

        {
            OptionalTiming timing(request.diagnostics
                ? &request.diagnostics->frame_publish_qpc : nullptr);
            auto frame = write.publish(request.timing);
            if (!frame) return UnifiedStageRenderResult::invalid_request;
            (void)hub.publish(frame);
        }
        ++rendered;
        return UnifiedStageRenderResult::published;
    }

    void retire() noexcept {
        if (!is_configured) return;
        // Renderer resources are shared across pool-slot contexts in one D2D
        // resource domain. Release the active target before the slots drop
        // their producer contexts.
        release_target_renderers();
        video_base_surface.retire_producer_access();
        invalidate_video_base();
        pool.retire_producer_access();
        is_configured = false;
    }
};

UnifiedStageRenderer::UnifiedStageRenderer()
    : impl_(std::make_unique<Impl>()) {}
UnifiedStageRenderer::~UnifiedStageRenderer() = default;
UnifiedStageRenderer::UnifiedStageRenderer(UnifiedStageRenderer&&) noexcept = default;
UnifiedStageRenderer& UnifiedStageRenderer::operator=(
    UnifiedStageRenderer&&) noexcept = default;

bool UnifiedStageRenderer::configure(const StageOutputProfile& profile,
        std::size_t frame_pool_capacity,
        void* native_d3d11_device) {
    return impl_ && impl_->configure(
        profile, frame_pool_capacity, native_d3d11_device);
}

bool UnifiedStageRenderer::configured() const noexcept {
    return impl_ && impl_->is_configured;
}

StageOutputProfile UnifiedStageRenderer::profile() const noexcept {
    return configured() ? impl_->profile : StageOutputProfile{};
}

void* UnifiedStageRenderer::native_d3d_device() noexcept {
    return configured() ? impl_->compositor.current_frame().d3d_device : nullptr;
}

StageFrameMailbox UnifiedStageRenderer::subscribe() {
    return impl_ ? impl_->hub.subscribe() : StageFrameMailbox{};
}

UnifiedStageRenderResult UnifiedStageRenderer::render(
        const UnifiedStageRenderRequest& request) {
    return impl_ ? impl_->render(request)
        : UnifiedStageRenderResult::invalid_request;
}

std::uint64_t UnifiedStageRenderer::rendered_frames() const noexcept {
    return impl_ ? impl_->rendered : 0;
}

std::uint64_t UnifiedStageRenderer::dropped_frames() const noexcept {
    return impl_ ? impl_->dropped : 0;
}

void UnifiedStageRenderer::retire() noexcept {
    if (impl_) impl_->retire();
}

} // namespace kirakara::show::win32
