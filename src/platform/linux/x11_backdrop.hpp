// SPDX-License-Identifier: MIT
//
// X11 OS backdrop integration for the window (glass-v1.md section 6). Private to
// src/platform/linux: the engine sees only hal::BackdropMaterial through WindowMetrics.
//
// X11 has no documented blur protocol. KWin honours the `_KDE_NET_WM_BLUR_BEHIND_REGION` window
// property when a compositing manager is running, which is the "blur on KWin" row of the table; any
// other window manager (and XWayland, whose surface belongs to the Wayland compositor rather than
// to this X11 window) resolves to no material and the stylesheet's opaque fallback applies.
//
// A translucent X11 window needs two things this file owns: a 32-bit TrueColor (ARGB) visual, so
// the swapchain's per-pixel alpha reaches the compositor, and the blur-behind property itself.

#pragma once

#include "tessera/hal/window_backdrop.hpp"

#include <X11/Xlib.h>

namespace tessera::hal::detail {

// The window attributes the translucent path needs, resolved before the window is created because
// the visual and depth are `XCreateWindow` inputs. On the opaque path every field is neutral.
struct X11BackdropSetup {
    BackdropCapabilities capabilities;
    // The ARGB visual and its colormap, or null/None when the opaque path stays in effect.
    Visual* visual = nullptr;
    Colormap colormap = None;
    // 0 means "inherit the parent's depth" (the opaque path).
    int depth = 0;
};

// Detects the server's backdrop facts once, at window creation. `translucent` selects the ARGB
// visual only when the request asks for translucency and the server has one; a compositing manager
// must be running for either to be visible.
[[nodiscard]] X11BackdropSetup prepare_x11_backdrop(Display* display, int screen,
                                                    bool translucent) noexcept;

// Applies the resolved material to `window`: the KWin blur-behind region for `Blur`, nothing for
// the other materials (X11 offers no Mica/Acrylic), and the documented opaque fallback when the
// server cannot present the request. Never blocks and never waits for the compositor.
[[nodiscard]] BackdropResolution
apply_x11_backdrop(Display* display, ::Window window, WindowBackdrop request, bool transparent,
                   const BackdropCapabilities& capabilities) noexcept;

// Extends the caller's `XSetWindowAttributes`/mask for the translucent path: a 32-bit ARGB visual
// needs its colormap and a transparent background/border pixel. A no-op on the opaque path, so the
// window creation call stays byte-identical for a project that never asks for glass.
void configure_x11_window_attributes(const X11BackdropSetup& setup,
                                     XSetWindowAttributes& attributes,
                                     unsigned long& mask) noexcept;

// Frees the colormap the translucent path created; a no-op on the opaque path.
void release_x11_backdrop(Display* display, X11BackdropSetup& setup) noexcept;

} // namespace tessera::hal::detail
