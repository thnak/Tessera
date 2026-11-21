// SPDX-License-Identifier: MIT
//
// The host's Back dispatch (docs/specs/studio-device-profiles-v1.md section 4, issue #445): a
// BackRequested input resolves against the topmost dismissible overlay, then the app's route-level
// handler, and is otherwise reported as unhandled for the embedding host to act on.

#include "host_impl.hpp"
#include "tessera/app/host.hpp"

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

} // namespace tessera::app
