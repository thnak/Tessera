// SPDX-License-Identifier: MIT
//
// The driver's IME anchor sync (issue #1053): the focused scene node's caret rect goes to the
// platform window so the input method's candidate window anchors at the caret. See
// window_text_input_rect.hpp for the geometry and the change-only rule.

#include "window_impl.hpp"

namespace tessera::app {

void WindowHost::Impl::sync_text_input_rect() {
    if (!available || window == nullptr || host == nullptr) {
        return;
    }
    const detail::TextStack* const stack = text();
    (void)detail::sync_text_input_rect(*window, pump.router().focus_manager().focused(),
                                       host->styles(),
                                       stack != nullptr ? stack->paint_metrics.get() : nullptr,
                                       device_scale, text_input_rect_state);
}

} // namespace tessera::app
