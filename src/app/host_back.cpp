// SPDX-License-Identifier: MIT
//
// The host's Back dispatch (docs/specs/studio-device-profiles-v1.md section 4, issue #445): a
// BackRequested input resolves against the topmost dismissible overlay, then the app's route-level
// handler, and is otherwise reported as unhandled for the embedding host to act on.

#include "host_impl.hpp"
#include "tessera/app/host.hpp"
#include "tessera/ui/key_code.hpp"

namespace tessera::app {

void AppHost::set_back_handler(BackHandler handler, void* user) {
    impl_->back_handler = handler;
    impl_->back_handler_user = user;
}

BackDispatch AppHost::last_back_dispatch() const {
    return impl_->last_back;
}

std::size_t AppHost::unhandled_back_count() const {
    return impl_->unhandled_backs;
}

bool AppHost::Impl::handle_back_request() {
    // Overlay first, then the route stack, then report: the order of navigation-v1.md section
    // 10.1. The request is always consumed here; the scene has no Back event.
    if (overlay_host && overlay_host->handle_back()) {
        last_back = BackDispatch::OverlayDismissed;
        loop.mark_render_dirty();
    } else if (back_handler != nullptr && back_handler(back_handler_user)) {
        last_back = BackDispatch::HandlerConsumed;
        loop.mark_render_dirty();
    } else {
        last_back = BackDispatch::Unhandled;
        ++unhandled_backs;
    }
    return true;
}

// The overlay side of the input path (docs/specs/overlays-and-layers-v1.md): Escape dismisses the
// topmost dismissible overlay and is consumed, a press outside closes the overlays that opted in
// and still reaches the scene, and window blur closes the transient ones. It lives beside the Back
// dispatch because both are "the overlay sees input first" rules.
bool AppHost::handle_overlay_input(const ui::InputEvent& event) {
    Impl& state = *impl_;
    if (event.type == ui::InputEventType::BackRequested) {
        return state.handle_back_request();
    }
    if (!state.overlay_host) {
        return false;
    }
    ui::OverlayHost& overlays = *state.overlay_host;
    switch (event.type) {
    case ui::InputEventType::KeyDown:
        if (event.key == ui::key::kEscape && overlays.handle_escape()) {
            state.escape_swallowed = true;
            state.loop.mark_render_dirty();
            return true;
        }
        return false;
    case ui::InputEventType::KeyUp:
        // The release that belongs to a consumed press must not reach a focused control alone.
        if (event.key == ui::key::kEscape && state.escape_swallowed) {
            state.escape_swallowed = false;
            return true;
        }
        return false;
    case ui::InputEventType::PointerDown:
        // The press still reaches whatever is under it; only the overlays close.
        if (overlays.handle_pointer_down(event.x, event.y) > 0) {
            state.loop.mark_render_dirty();
        }
        return false;
    case ui::InputEventType::Blur:
        if (overlays.close_transient() > 0) {
            state.loop.mark_render_dirty();
        }
        return false;
    default:
        return false;
    }
}

} // namespace tessera::app
