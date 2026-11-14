// SPDX-License-Identifier: MIT
//
// Wayland window backend factory (issue #155). The implementation lives in window_wayland.cpp; this
// header lets the Linux backend selector (window_linux.cpp) call it without pulling wayland-client.

#pragma once

#include "tessera/hal/window.hpp"

#include <memory>

namespace tessera::hal::detail {

// Connects to the compositor named by `WAYLAND_DISPLAY` and creates the window. Returns nullptr
// when the session has no compositor, the connection fails, a required global is missing or the
// descriptor is invalid; exactly one TESSERA_WINDOW_WAYLAND_UNSUPPORTED diagnostic is printed in
// that case (the caller may still fall back to X11).
std::unique_ptr<Window> create_wayland_window(const WindowDesc& desc);

} // namespace tessera::hal::detail
