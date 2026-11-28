// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/runtime/timer_queue.hpp"

// Decides when the browser host may call `requestAnimationFrame` (ADR-046 item 5). Pure state: no
// Emscripten, no clock. The rule it enforces is the issue's acceptance, "a static page idles with
// zero frame callbacks": a callback is requested only on a wake (input, timer, completion, resize,
// visibility) or while the last tick says work is still pending, and never twice at once.

namespace tessera::hal::web {

// Retry interval while the device is lost or recovering: never spin a rAF loop on a dead device.
inline constexpr double kDeviceBlockedRetrySeconds = 0.1;

struct FrameDecision {
    // Call `requestAnimationFrame` now.
    bool request_frame = false;
    // Otherwise, arm one `setTimeout` for this many seconds that wakes the host; kNoDeadline means
    // the page parks with no callback of any kind (input and the next wake re-arm it).
    double wake_after_seconds = runtime::kNoDeadline;
};

class FrameCallbackScheduler {
public:
    // Something happened that may need a frame. True when the caller must request one now (none is
    // outstanding and the page is visible); false when one is already coming or the page is hidden.
    // A wake while hidden is remembered and answered by the next `set_visible(true)`.
    [[nodiscard]] bool wake();

    // The `requestAnimationFrame` callback fired: nothing is outstanding until the decision says
    // so.
    void begin_callback();

    // The loop ran one tick. `timer_wait_seconds` is `RuntimeLoop::next_timer_wait_seconds()`.
    [[nodiscard]] FrameDecision end_callback(const runtime::RuntimeTick& tick,
                                             double timer_wait_seconds);

    // Visibility is a lifecycle event. Hidden: no callback is requested (the browser would pause
    // them anyway). Becoming visible always repaints once. Returns true when a frame must be
    // requested now.
    [[nodiscard]] bool set_visible(bool visible);

    [[nodiscard]] bool visible() const {
        return visible_;
    }
    [[nodiscard]] bool callback_outstanding() const {
        return outstanding_;
    }

private:
    bool visible_ = true;
    bool outstanding_ = false;
    bool pending_while_hidden_ = false;
};

} // namespace tessera::hal::web
