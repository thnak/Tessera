// SPDX-License-Identifier: MIT
//
// Bounded libwayland dispatch (issue #155). See window_wayland_dispatch.hpp for the contract.

#include "tessera/hal/timer/steady_time.hpp"
#include "window_wayland_dispatch.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <poll.h>
#include <wayland-client.h>

namespace tessera::hal::detail {

WaylandConnection::~WaylandConnection() {
    disconnect();
}

bool WaylandConnection::connect() {
    if (display_ != nullptr) {
        return true;
    }
    // A null name lets libwayland read the environment, which is what makes an injected socket path
    // (the tests) work without this file knowing about it.
    display_ = wl_display_connect(nullptr);
    return display_ != nullptr;
}

bool WaylandConnection::pump() {
    if (display_ == nullptr || lost_) {
        return false;
    }
    if (wl_display_dispatch_pending(display_) < 0) {
        lost_ = true;
        return false;
    }
    if (wl_display_flush(display_) < 0 && errno != EAGAIN) {
        lost_ = true;
        return false;
    }
    return true;
}

bool WaylandConnection::wait_once(double deadline) {
    if (display_ == nullptr || lost_) {
        return false;
    }
    while (wl_display_prepare_read(display_) != 0) {
        if (errno != EAGAIN) {
            lost_ = true;
            return false;
        }
        // Events are already queued: dispatch them instead of waiting for more.
        if (wl_display_dispatch_pending(display_) < 0) {
            lost_ = true;
            return false;
        }
    }
    if (wl_display_flush(display_) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(display_);
        lost_ = true;
        return false;
    }
    const double remaining = seconds_until(steady_now_seconds(), deadline);
    if (remaining <= 0.0) {
        wl_display_cancel_read(display_);
        return true;
    }
    // Whole milliseconds, rounded up and never zero, so a sub-millisecond remainder parks once
    // instead of spinning; clamped so the wait stays bounded.
    const double wait_milliseconds = std::max(1.0, std::ceil(remaining * 1000.0));
    const int milliseconds = wait_milliseconds >= static_cast<double>(INT_MAX)
                                 ? INT_MAX
                                 : static_cast<int>(wait_milliseconds);
    pollfd descriptor{};
    descriptor.fd = wl_display_get_fd(display_);
    descriptor.events = POLLIN;
    const int result = poll(&descriptor, 1, milliseconds);
    if (result > 0) {
        // read_events() observes a disconnect as -1/EPIPE, so it is called even when the same poll
        // reports POLLERR/POLLHUP.
        if (wl_display_read_events(display_) < 0) {
            lost_ = true;
            return false;
        }
    } else {
        wl_display_cancel_read(display_);
        if (result == 0) {
            return true; // Timed out: the caller decides whether the budget is spent.
        }
        if (errno != EINTR) {
            // A poll failure never becomes a spin: idle out the remaining budget once and report.
            wait_until_seconds(deadline);
            return true;
        }
    }
    // read_events() only queues what the socket delivered; dispatch_pending() is what invokes the
    // listeners (registry globals, xdg-shell configure, ping).
    if (wl_display_dispatch_pending(display_) < 0) {
        lost_ = true;
        return false;
    }
    return true;
}

void WaylandConnection::disconnect() noexcept {
    if (display_ == nullptr) {
        return;
    }
    (void)wl_display_flush(display_);
    wl_display_disconnect(display_);
    display_ = nullptr;
    lost_ = false;
}

} // namespace tessera::hal::detail
