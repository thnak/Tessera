// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"

namespace tessera::ui {

// Wheel input plus the scroll default action (docs/specs/scroll-and-gestures-v1.md section 3).
// Dispatches the wheel event to the deepest hit node through the normal capture/target/bubble path
// and, unless a handler set `prevent_default`, scrolls the nearest scrollable ancestor (scroll.hpp)
// by the deltas converted to logical pixels, chaining the remainder outward while each container's
// `overscroll-behavior` allows it. A wheel over a non-scrollable chain dispatches exactly as before
// and changes nothing.
InputDispatchResult dispatch_wheel(Node& root, const InputEvent& input);

} // namespace tessera::ui
