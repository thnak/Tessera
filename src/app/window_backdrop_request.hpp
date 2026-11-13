// SPDX-License-Identifier: MIT
//
// The startup OS backdrop request of one window (docs/specs/glass-v1.md sections 2 and 6). Private
// to src/app and part of the window driver target, so the host-only `tessera_app` library keeps its
// documented "never touches a window" boundary while the driver can read the root style and hand
// the request to the platform HAL.

#pragma once

#include "tessera/hal/window_backdrop.hpp"

namespace tessera::ui {
class Node;
}

namespace tessera::app {

// An explicit `WindowOptions::backdrop` wins; otherwise the root node - the `Window` element - may
// carry `window-backdrop` as inline style before the host exists. A stylesheet rule needs the
// startup manifest's resolved request (a later landing), so only the inline form is read here. An
// unknown value keeps the documented `None` default instead of failing startup.
[[nodiscard]] hal::WindowBackdrop window_backdrop_request(const ui::Node& root,
                                                          hal::WindowBackdrop explicit_request);

} // namespace tessera::app
