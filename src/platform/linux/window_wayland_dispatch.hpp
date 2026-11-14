// SPDX-License-Identifier: MIT
//
// Bounded event dispatch for the Wayland window backend (issue #155).
//
// The canonical libwayland client loop is a protocol: `wl_display_prepare_read`,
// `wl_display_flush`, `poll` on the display fd, `wl_display_read_events` (or `cancel_read` on a
// timeout), then `wl_display_dispatch_pending` to invoke the listeners. It is easy to get subtly
// wrong - omitting the final dispatch queues events and invokes nothing, which looks exactly like a
// dead compositor - so it lives here as one small type both the bring-up handshake and the runtime
// wait loop use.
//
// Two invariants: no call blocks longer than the poll timeout it is given (ADR-021, the UI thread
// is never parked on an OS source without a bound), and a dead connection is reported as a value
// once instead of being retried forever.

#pragma once

struct wl_display;

namespace tessera::hal::detail {

class WaylandConnection final {
public:
    WaylandConnection() = default;
    ~WaylandConnection();

    WaylandConnection(const WaylandConnection&) = delete;
    WaylandConnection& operator=(const WaylandConnection&) = delete;

    // Connects to the compositor named by the environment (`WAYLAND_DISPLAY`). False when there is
    // no compositor to reach; the caller owns the diagnostic.
    bool connect();

    // Dispatches already-queued events and flushes outgoing requests. False once the connection is
    // gone (EPIPE after a compositor-side protocol error or a stopped compositor); the caller turns
    // that into a Close.
    bool pump();

    // One bounded slice: wait at most until `deadline` (monotonic seconds) for the compositor, then
    // dispatch whatever arrived. False when the connection ended.
    bool wait_once(double deadline);

    // True after a failed read/write. Sticky: a lost Wayland connection cannot be resumed.
    [[nodiscard]] bool lost() const noexcept {
        return lost_;
    }

    [[nodiscard]] wl_display* display() const noexcept {
        return display_;
    }

    // Flushes and disconnects. Safe twice, and safe on a connection that never succeeded.
    void disconnect() noexcept;

    // Abandons the connection without closing it. Only for a foreign-thread teardown: the owning
    // thread still owns the fd and its proxies, so closing it from another thread would race the
    // dispatch loop. Leaking a connection at process teardown is the bounded alternative.
    void leak() noexcept {
        display_ = nullptr;
        lost_ = false;
    }

private:
    wl_display* display_ = nullptr;
    bool lost_ = false;
};

} // namespace tessera::hal::detail
