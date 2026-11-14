// SPDX-License-Identifier: MIT
//
// xdg-shell role stack of the Wayland window backend (issue #155). See
// window_wayland_internal.hpp for the normative rules implemented here.

#include "window_wayland_internal.hpp"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <wayland-client.h>

namespace tessera::hal::detail {
namespace {

// The xdg-shell version this backend speaks. The generated code is bound at the version the
// compositor advertises (clamped to this): asking for a newer version than the compositor has is a
// protocol error, and 1 is enough for surface+toplevel+configure. Version 6 is where the
// "suspended" state appears, which is why the state parser also accepts it only when advertised.
constexpr std::uint32_t kXdgShellVersion = 6;

} // namespace

// The wl_registry/wl_compositor listeners have no state of their own beyond the shell object, so
// they live in one named struct instead of being anonymous functions the header cannot name.
struct WaylandShellCallbacks {
    static void global(void* data, wl_registry* registry, std::uint32_t name, const char* interface,
                       std::uint32_t version) {
        static_cast<WaylandShell*>(data)->on_global(registry, name, interface, version);
    }

    static void global_remove(void*, wl_registry*, std::uint32_t) {
        // A global going away (a compositor stopping an output, a seat disappearing) does not
        // invalidate the objects this backend already created: wl_compositor and xdg_wm_base are
        // not removable globals. Nothing to do.
    }

    static void wm_base_ping(void*, xdg_wm_base* wm_base, std::uint32_t serial) {
        // MANDATORY. A compositor disconnects a client that ignores a ping ("unresponsive"
        // error); answering is cheap and must never wait for the app. The data pointer is unused:
        // the reply is the protocol object the event carries.
        xdg_wm_base_pong(wm_base, serial);
    }

    static void surface_configure(void* data, xdg_surface* surface, std::uint32_t serial) {
        static_cast<WaylandShell*>(data)->on_surface_configure(surface, serial);
    }

    static void toplevel_configure(void* data, xdg_toplevel*, std::int32_t width,
                                   std::int32_t height, wl_array* states) {
        static_cast<WaylandShell*>(data)->on_toplevel_configure(width, height, states);
    }

    static void toplevel_close(void* data, xdg_toplevel*) {
        static_cast<WaylandShell*>(data)->on_toplevel_close();
    }

    // Optional toplevel events. configure_bounds and wm_capabilities are informational and this
    // slice does not act on them; they are present in the listener struct from xdg-shell 4/5, and a
    // null entry is legal for a client that speaks an older version, so they stay empty.
    static void toplevel_configure_bounds(void*, xdg_toplevel*, std::int32_t, std::int32_t) {}
    static void toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}
};

namespace {

const wl_registry_listener kRegistryListener = {
    WaylandShellCallbacks::global,
    WaylandShellCallbacks::global_remove,
};

const xdg_wm_base_listener kWmBaseListener = {WaylandShellCallbacks::wm_base_ping};

const xdg_surface_listener kXdgSurfaceListener = {WaylandShellCallbacks::surface_configure};

const xdg_toplevel_listener kToplevelListener = {
    WaylandShellCallbacks::toplevel_configure,
    WaylandShellCallbacks::toplevel_close,
    WaylandShellCallbacks::toplevel_configure_bounds,
    WaylandShellCallbacks::toplevel_wm_capabilities,
};

} // namespace

WaylandShell::~WaylandShell() {
    destroy();
}

bool WaylandShell::connect(wl_display* display) {
    if (display == nullptr || registry_ != nullptr) {
        return false;
    }
    display_ = display;
    registry_ = wl_display_get_registry(display);
    if (registry_ == nullptr) {
        return false;
    }
    return wl_registry_add_listener(registry_, &kRegistryListener, this) == 0;
}

void WaylandShell::on_global(wl_registry* registry, std::uint32_t name, const char* interface,
                             std::uint32_t version) {
    if (interface == nullptr) {
        return;
    }
    if (std::string_view(interface) == wl_compositor_interface.name && compositor_ == nullptr) {
        // wl_compositor 4 adds damage_buffer, which this backend does not need; binding a version
        // the compositor supports is enough.
        compositor_ = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4U)));
        return;
    }
    if (std::string_view(interface) == xdg_wm_base_interface.name && wm_base_ == nullptr) {
        wm_base_ = static_cast<xdg_wm_base*>(wl_registry_bind(
            registry, name, &xdg_wm_base_interface, std::min(version, kXdgShellVersion)));
        if (wm_base_ != nullptr) {
            xdg_wm_base_add_listener(wm_base_, &kWmBaseListener, this);
        }
    }
}

bool WaylandShell::create_roles(const char* title) {
    if (display_ == nullptr || !globals_ready() || surface_ != nullptr) {
        return false;
    }
    surface_ = wl_compositor_create_surface(compositor_);
    if (surface_ == nullptr) {
        return false;
    }
    xdg_surface_ = xdg_wm_base_get_xdg_surface(wm_base_, surface_);
    if (xdg_surface_ == nullptr) {
        return false;
    }
    xdg_surface_add_listener(xdg_surface_, &kXdgSurfaceListener, this);
    toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
    if (toplevel_ == nullptr) {
        return false;
    }
    xdg_toplevel_add_listener(toplevel_, &kToplevelListener, this);
    if (title != nullptr && title[0] != '\0') {
        xdg_toplevel_set_title(toplevel_, title);
    }
    // An explicit app id is what makes a compositor group windows and match a .desktop file; the
    // framework name is the honest default until per-app ids exist.
    xdg_toplevel_set_app_id(toplevel_, "dev.tessera.app");
    // Committing a role-less-surface-with-a-role is what asks the compositor for the initial
    // configure sequence. No buffer is attached (the documented "never draw before ack" rule).
    wl_surface_commit(surface_);
    if (wl_display_flush(display_) < 0) {
        return false;
    }
    return true;
}

void WaylandShell::on_surface_configure(xdg_surface* surface, std::uint32_t serial) {
    // Ack every sequence, including one whose toplevel.configure never arrived (the compositor may
    // send only the xdg_surface one). The ack must happen before the next buffer commit.
    if (surface == nullptr) {
        return;
    }
    xdg_surface_ack_configure(surface, serial);
    configured_ = true;
    if (pending_valid_) {
        state_ = pending_;
        pending_valid_ = false;
        configure_ready_ = true;
    }
}

void WaylandShell::on_toplevel_configure(std::int32_t width, std::int32_t height,
                                         const wl_array* states) {
    // Start a fresh sequence: xdg_toplevel.configure always precedes the xdg_surface.configure that
    // ends this sequence.
    pending_ = WaylandConfigureState{};
    pending_.width = width;
    pending_.height = height;
    pending_valid_ = true;
    if (states == nullptr) {
        return;
    }
    const auto* values = static_cast<const std::uint32_t*>(states->data);
    const std::size_t count = states->size / sizeof(std::uint32_t);
    for (std::size_t i = 0; i < count; ++i) {
        switch (values[i]) {
        case XDG_TOPLEVEL_STATE_ACTIVATED:
            pending_.activated = true;
            break;
        case XDG_TOPLEVEL_STATE_MAXIMIZED:
            pending_.maximized = true;
            break;
        case XDG_TOPLEVEL_STATE_FULLSCREEN:
            pending_.fullscreen = true;
            break;
        case XDG_TOPLEVEL_STATE_SUSPENDED:
            pending_.suspended = true;
            break;
        default:
            // resizing/tiled/constrained change nothing this backend reports; unknown values are
            // ignored instead of being invented into an event.
            break;
        }
    }
}

void WaylandShell::on_toplevel_close() {
    close_requested_ = true;
}

const WaylandConfigureState* WaylandShell::consume_configure() noexcept {
    if (!configure_ready_) {
        return nullptr;
    }
    configure_ready_ = false;
    return &state_;
}

bool WaylandShell::consume_close_request() noexcept {
    const bool requested = close_requested_;
    close_requested_ = false;
    return requested;
}

void WaylandShell::destroy() noexcept {
    // Reverse creation order. Each destroy flushes the request; the display is disconnected by the
    // window owner afterwards.
    if (toplevel_ != nullptr) {
        xdg_toplevel_destroy(toplevel_);
        toplevel_ = nullptr;
    }
    if (xdg_surface_ != nullptr) {
        xdg_surface_destroy(xdg_surface_);
        xdg_surface_ = nullptr;
    }
    if (surface_ != nullptr) {
        wl_surface_destroy(surface_);
        surface_ = nullptr;
    }
    if (compositor_ != nullptr) {
        wl_compositor_destroy(compositor_);
        compositor_ = nullptr;
    }
    if (wm_base_ != nullptr) {
        xdg_wm_base_destroy(wm_base_);
        wm_base_ = nullptr;
    }
    if (registry_ != nullptr) {
        wl_registry_destroy(registry_);
        registry_ = nullptr;
    }
    display_ = nullptr;
    pending_valid_ = false;
    configure_ready_ = false;
    close_requested_ = false;
}

} // namespace tessera::hal::detail
