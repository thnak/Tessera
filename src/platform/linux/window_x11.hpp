// SPDX-License-Identifier: MIT
//
// X11 window backend factory (issue #155). The X11 implementation lives in window.cpp; this tiny
// header lets the Linux backend selector (window_linux.cpp) call it without including Xlib.

#pragma once

#include "tessera/hal/window.hpp"

#include <memory>

namespace tessera::hal::detail {

// Creates the X11 window, or nullptr when the X server is unreachable or the descriptor is invalid.
std::unique_ptr<Window> create_x11_window(const WindowDesc& desc);

} // namespace tessera::hal::detail
