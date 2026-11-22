// SPDX-License-Identifier: MIT
//
// The glue between AppHost::Impl and detail::QualityStage (god-type audit slice 10, section 3.4).
//
// The stage takes function pointers plus a context, which is what makes it testable without a host.
// Something has to bind those pointers to the real host state, and that something is this file:
// four small adapters, kept out of host.cpp because that translation unit is already at its size
// budget and these are the only definitions in the host that exist purely for the stage.
//
// They are static members of Impl rather than file-local functions because Impl is a private nested
// type, so a free function could not name it.

#include "host_impl.hpp"
#include "tessera/ui/scene/node.hpp"

namespace tessera::app {
namespace {

// True when some visible node paints a blurred outer shadow: the effect the first ladder position
// reduces. Read from the outsets the style sync already stores on each node, so it needs no style
// resolution. Allocation-free; it runs only on the observation that changes the ladder position.
bool scene_has_blurred_shadow(const ui::Node& node) noexcept {
    if (!node.rendered()) {
        return false;
    }
    if (node.paint_outsets().blurred) {
        return true;
    }
    for (const auto& child : node.children()) {
        if (scene_has_blurred_shadow(*child)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool AppHost::Impl::scene_has_blurred_shadow_callback(const void* context) noexcept {
    return scene_has_blurred_shadow(static_cast<const AppHost::Impl*>(context)->root);
}

bool AppHost::Impl::non_essential_animations_callback(const void* context) noexcept {
    return static_cast<const AppHost::Impl*>(context)->animations.non_essential_active() > 0;
}

// Reduced effects change pixels, and a settled on-demand scene has nothing else to repaint.
void AppHost::Impl::set_reduce_effects_callback(void* context, bool reduced) {
    AppHost::Impl& state = *static_cast<AppHost::Impl*>(context);
    if (state.pipeline->reduce_effects() == reduced) {
        return;
    }
    state.pipeline->set_reduce_effects(reduced);
    state.full_repaint_pending = true;
    state.loop.mark_render_dirty();
}

// Pausing is a value change; resuming must wake a loop that went idle while animations were frozen.
void AppHost::Impl::set_animations_paused_callback(void* context, bool paused) {
    AppHost::Impl& state = *static_cast<AppHost::Impl*>(context);
    if (state.animations.suspend_non_essential() == paused) {
        return;
    }
    state.animations.set_suspend_non_essential(paused);
    state.loop.mark_render_dirty();
}

} // namespace tessera::app
