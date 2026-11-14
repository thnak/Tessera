// SPDX-License-Identifier: MIT
//
// Private xdg-shell role stack of the Wayland window backend (issue #155).
//
// The window backend (window_wayland.cpp) owns the wl_display connection, the normalized event
// queue and the bounded dispatch loop. This header owns the protocol objects a toplevel needs - the
// wl_compositor/xdg_wm_base globals, the wl_surface, the xdg_surface and the xdg_toplevel - and the
// configure state machine. Splitting it keeps both translation units inside the file-size budget.
//
// Normative rules this file implements:
//  - xdg_wm_base ping -> pong is answered immediately; a client that does not pong is disconnected
//    (docs/design/hal.md section 5, xdg-shell "unresponsive" error).
//  - The compositor owns the size. xdg_toplevel.configure carries width/height (0 = client
//    chooses); xdg_surface.configure ends a configure sequence and carries the serial to ack.
//    A client must ack_configure before it commits a buffer with the new size, so the ack happens
//    here, inside the callback, for every configure sequence - never lazily (docs/design/hal.md
//    section 6: "never draw before ack_configure").
//  - Only the documented toplevel states are interpreted (activated, maximized, fullscreen,
//    suspended). Unknown enum values are ignored, not invented into events.

#pragma once

#include <cstdint>

struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_surface;
struct wl_array;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;

namespace tessera::hal::detail {

// One configure sequence, in logical pixels. A non-positive width/height is the compositor
// delegating the size to the client.
struct WaylandConfigureState {
    std::int32_t width = 0;
    std::int32_t height = 0;
    bool activated = false;
    bool maximized = false;
    bool fullscreen = false;
    // xdg_toplevel state "suspended" (since xdg-shell 6): the surface is not being repainted
    // (another window covers it, outputs are off, screen locked). It is the compositor's occlusion
    // signal and maps to WindowEventType::Occluded / WindowMetrics::occluded.
    bool suspended = false;
};

class WaylandShell final {
public:
    WaylandShell() = default;
    ~WaylandShell();

    WaylandShell(const WaylandShell&) = delete;
    WaylandShell& operator=(const WaylandShell&) = delete;

    // Creates the wl_registry and starts binding the globals this backend uses. The caller must
    // dispatch the display (bounded) until globals_ready() before create_roles().
    bool connect(wl_display* display);

    // True once both wl_compositor and xdg_wm_base are bound. A compositor that lacks one never
    // becomes ready, and the caller reports the unsupported path once.
    [[nodiscard]] bool globals_ready() const noexcept {
        return compositor_ != nullptr && wm_base_ != nullptr;
    }

    // Creates the wl_surface/xdg_surface/xdg_toplevel stack and commits the surface, which is what
    // makes the compositor send the initial configure sequence. Returns false when a global is
    // missing or a protocol object could not be created.
    bool create_roles(const char* title);

    // The newest configure sequence acked since the last call, or nullptr when nothing new arrived.
    // The app layer compares it against its current state to normalize Resize/Focus/Blur/Occluded.
    [[nodiscard]] const WaylandConfigureState* consume_configure() noexcept;

    // True exactly once per xdg_toplevel.close from the compositor (the user clicked the close
    // button, a WM shortcut, a session logout). Consumed like consume_configure().
    [[nodiscard]] bool consume_close_request() noexcept;

    // True once at least one configure sequence has been acked: before that the surface must not
    // have a buffer attached.
    [[nodiscard]] bool configured() const noexcept {
        return configured_;
    }

    [[nodiscard]] wl_surface* surface() const noexcept {
        return surface_;
    }

    // Destroys the role stack and the globals in the documented teardown order (toplevel,
    // xdg_surface, wl_surface, compositor, wm_base, registry). Safe to call twice and safe on a
    // partially created stack.
    void destroy() noexcept;

private:
    friend struct WaylandShellCallbacks;

    void on_global(wl_registry* registry, std::uint32_t name, const char* interface,
                   std::uint32_t version);
    void on_surface_configure(xdg_surface* surface, std::uint32_t serial);
    void on_toplevel_configure(std::int32_t width, std::int32_t height, const wl_array* states);
    void on_toplevel_close();

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    xdg_wm_base* wm_base_ = nullptr;
    wl_surface* surface_ = nullptr;
    xdg_surface* xdg_surface_ = nullptr;
    xdg_toplevel* toplevel_ = nullptr;
    // The configure sequence being collected: xdg_toplevel.configure fills it, the following
    // xdg_surface.configure acks and publishes it into `state_`.
    WaylandConfigureState pending_;
    WaylandConfigureState state_;
    bool pending_valid_ = false;
    bool configured_ = false;
    bool configure_ready_ = false;
    bool close_requested_ = false;
};

} // namespace tessera::hal::detail
