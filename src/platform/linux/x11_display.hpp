// SPDX-License-Identifier: MIT
//
// X11 display-capability reads for one window (docs/specs/os-events-v1.md section 5; ADR-023
// rules 4-7).
//
// X11 has no per-window scale query and no per-window refresh query: the scale is the desktop's
// `Xft.dpi` Xresource (or a RandR transform) and the refresh is the active CRTC's mode. This header
// is the platform half only; it reports what it could read and sets the matching `*_assumed` flag
// when it could not, and `window.cpp`/`x11_display.cpp` fold the result into the event stream.
// `window.cpp` is over the file-size limit, so the display reads live here, split across two
// translation units: `x11_display.cpp` (DPI, work area, event normalization) and
// `x11_randr_monitors.cpp` (every XRR* query, compiled only with libXrandr).
//
// Xlib defines `Window` and `CursorShape` as macros, and `None`/`Bool`/`Status` collide with
// ordinary identifiers. The HAL headers are parsed before Xlib everywhere in this directory (see
// window.cpp's include comment); this header keeps the same rule and spells `::Window`.

#pragma once

#include "tessera/hal/display.hpp"
#include "tessera/hal/window.hpp"

#include <X11/Xlib.h>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tessera::hal::detail {

// --- DPI ---------------------------------------------------------------------------------------
//
// X11 has no per-window DPI query: the desktop publishes a global `Xft.dpi` Xresource. These pure
// helpers are shared by the window's metrics build and its creation path; they live here (with the
// display reads) so `window.cpp` stays inside its file-size budget.

inline constexpr int kDefaultX11Dpi = 96;
// `Xft.dpi` is user-controlled; clamp obviously broken values so a typo cannot turn every logical
// size into zero or a multi-thousand-pixel window.
inline constexpr int kMinX11Dpi = 48;
inline constexpr int kMaxX11Dpi = 768;

// The desktop's `Xft.dpi`, or `kDefaultX11Dpi` when the resource server is absent (a bare Xvfb, a
// minimal window manager). Clamped to the sane bounds above.
[[nodiscard]] int query_x11_xft_dpi(Display* display) noexcept;

// dpi / 96, the device scale every logical/physical conversion uses.
[[nodiscard]] float x11_dpi_scale(int dpi) noexcept;

// Physical pixels -> logical pixels at `dpi`, rounded to nearest.
[[nodiscard]] std::uint32_t x11_logical_from_physical(std::uint32_t physical, int dpi) noexcept;

// Logical pixels -> physical pixels at `dpi`, rounded to nearest.
[[nodiscard]] std::uint32_t x11_physical_from_logical(std::uint32_t logical, int dpi) noexcept;

// --- RandR (x11_randr_monitors.cpp) ------------------------------------------------------------
//
// These are defined only when the build found libXrandr; callers are always behind the
// `TESSERA_X11_RANDR` guard, and `x11_display.cpp` never calls them from an unguarded path.

// True when the server has RandR >= 1.5 (the version that introduced `XRRGetMonitors`), with the
// extension's event base optionally returned for the caller's event-type checks.
[[nodiscard]] bool randr_query_available(Display* display, int* event_base_out = nullptr) noexcept;

// The display the window's centre is on (else the primary output, else the first): its stable id
// and the active CRTC's refresh. Returns false when RandR cannot describe it, leaving both outputs
// at their "not known" values.
[[nodiscard]] bool query_x11_active_display(Display* display, ::Window window, ::Window root,
                                            DisplayId& id, double& refresh_hz) noexcept;

// The RandR monitor list: `query_x11_displays` with the extension guard resolved. Returns 0 without
// libXrandr or on a server older than RandR 1.5.
std::size_t query_x11_randr_displays(Display* display, ::Window root,
                                     std::span<DisplayInfo> out) noexcept;

// Selects the RandR notifications (screen change + notify) on `window` when the extension is
// present. Called once at creation, after the window exists; a server without RandR is a no-op and
// the window simply never receives a display event. Must run on the owning thread.
void subscribe_x11_display_events(Display* display, ::Window root) noexcept;

// --- Display capabilities ----------------------------------------------------------------------

// The display capabilities of the monitor `window` is on, as this build can read them, folded into
// `metrics`. A field it could not read keeps the documented fallback and its `*_assumed` flag.
// Without libXrandr (`TESSERA_X11_RANDR`) only the work area is readable from `_NET_WORKAREA`; the
// refresh and per-output scale stay assumed. Never blocks on the server: every call is an Xlib
// request answered from the local connection, not a round trip that waits on a compositor.
void fill_x11_display_capabilities(Display* display, ::Window window, ::Window root, int screen,
                                   WindowMetrics& metrics) noexcept;

// Fills `out` with the RandR monitor list and returns the count written (at most `kMaxDisplays`).
// Without libXrandr, or on a server older than RandR 1.5, returns 0 and leaves `out` untouched: a
// backend with no topology source reports no per-monitor information rather than a fabricated one.
std::size_t query_x11_displays(Display* display, ::Window root,
                               std::span<DisplayInfo> out) noexcept;

// The display id of the output/CRTC the window's centre is on, or kNoDisplayId. Stable for the
// output's lifetime: the id is a hash of the output name, the same value `query_x11_displays`
// reports, so an add/remove event names the display the topology list describes.
[[nodiscard]] DisplayId x11_display_id(Display* display, ::Window window, ::Window root) noexcept;

// True when `event` is a RandR topology/mode notification that makes a re-query necessary. The
// window calls `XRRUpdateConfiguration` first (the RandR requirement) and then records the event.
[[nodiscard]] bool x11_event_is_display_change(const XEvent& event) noexcept;

// The normalized event a RandR notification implies: an output change that adds or removes a
// monitor is `DisplayAdded`/`DisplayRemoved`, a CRTC/mode change is `RefreshRateChanged`, anything
// else is `DisplayChanged`.
[[nodiscard]] WindowEventType x11_display_event_for(const XEvent& event) noexcept;

// `_NET_WORKAREA` for the window's desktop, in logical pixels: the EWMH property a window manager
// publishes when a strut/dock changes. Returns false and leaves `out` untouched when no window
// manager published it (a bare Xvfb has none), which leaves the documented fallback in place.
[[nodiscard]] bool query_x11_work_area(Display* display, ::Window root, int screen,
                                       WindowRect& out) noexcept;

} // namespace tessera::hal::detail
