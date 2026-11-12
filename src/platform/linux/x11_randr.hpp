// SPDX-License-Identifier: MIT
//
// RandR-backed display-refresh query for the X11 window (docs/specs/runtime-config-v1.md section
// 6.2). This is the only X11 piece that knows about RandR: the window calls it once (the host
// resolves WindowOptions::refresh_hz at creation), and it answers "unknown" whenever the server
// cannot describe the mode's timing. Compiled only when the build found libXrandr
// (TESSERA_X11_RANDR); the window otherwise returns the unknown value without calling it, and a
// server without the extension is caught at runtime by XRRQueryExtension.

#pragma once

#include <X11/Xlib.h>

namespace tessera::hal::detail {

// The refresh rate in Hz of the active CRTC the window is on, or kUnknownRefreshHz (0) when RandR
// is absent, older than 1.2, or the active mode has no usable timing (Xvfb's synthetic modes
// report a zero dot clock). Must be called on the window's owning thread.
double query_x11_display_refresh_hz(Display* display, ::Window window, ::Window root) noexcept;

} // namespace tessera::hal::detail
