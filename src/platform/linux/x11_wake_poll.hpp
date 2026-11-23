// SPDX-License-Identifier: MIT
//
// The X11 idle poll with the completion wake joined beside the display fd (issue #842).
//
// Split out of `window.cpp`, which is at its size budget: the wait loop there only needs the
// decision "display ready / wake signalled / timed out / error", not the descriptor bookkeeping.
//
// The display fd stays first so the existing display-error handling is unchanged when no wake is
// installed. The wake fd is only added when one exists, so a host that never installs a wake gets
// the display-only poll it had before, byte for byte.

#pragma once

#include <poll.h>

namespace tessera::hal::detail {

enum class X11PollResult : unsigned char {
    // The display is readable: drain and re-check for normalized events.
    Display,
    // The completion wake fired: a completion is queued, and the caller returns so the runtime
    // loop drains it on this thread.
    Wake,
    Timeout,
    // POLLERR/POLLHUP/POLLNVAL on the display, or poll() itself failed.
    Error,
};

// Polls `display_fd` and, when `wake_fd` is non-negative, the wake beside it. `milliseconds` is
// already clamped and rounded up by the caller.
inline X11PollResult x11_poll_for_events(int display_fd, int wake_fd, int milliseconds) {
    pollfd descriptors[2] = {};
    const nfds_t count = wake_fd >= 0 ? 2 : 1;
    descriptors[0].fd = display_fd;
    descriptors[0].events = POLLIN;
    if (wake_fd >= 0) {
        descriptors[1].fd = wake_fd;
        descriptors[1].events = POLLIN;
    }
    const int result = ::poll(descriptors, count, milliseconds);
    if (result > 0) {
        if (wake_fd >= 0 && (descriptors[1].revents & POLLIN) != 0) {
            return X11PollResult::Wake;
        }
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return X11PollResult::Error;
        }
        return X11PollResult::Display;
    }
    return result == 0 ? X11PollResult::Timeout : X11PollResult::Error;
}

} // namespace tessera::hal::detail
