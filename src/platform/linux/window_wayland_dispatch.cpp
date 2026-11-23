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
    // The display and, when the host installed one, the completion wake (issue #842). A wake means
    // a completion is queued: dismiss the read so the connection is not left half-read, then return
    // so the runtime loop drains the queue on its own thread.
    const int wake_fd = wake_ != nullptr ? wake_->file_descriptor() : -1;
    pollfd descriptors[2] = {};
    const nfds_t count = wake_fd >= 0 ? 2 : 1;
    descriptors[0].fd = wl_display_get_fd(display_);
    descriptors[0].events = POLLIN;
    if (wake_fd >= 0) {
        descriptors[1].fd = wake_fd;
        descriptors[1].events = POLLIN;
    }
    const int result = poll(descriptors, count, milliseconds);
    if (result > 0 && wake_fd >= 0 && (descriptors[1].revents & POLLIN) != 0) {
        wake_->drain();
        wl_display_cancel_read(display_);
        return true;
    }
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
