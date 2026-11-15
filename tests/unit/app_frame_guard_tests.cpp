// SPDX-License-Identifier: MIT

#include "tessera/app/frame_guard.hpp"

#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "frame_guard: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    using tessera::app::FrameFailureGuard;

    // Default limit: the run of failures is counted and trips exactly at the limit.
    FrameFailureGuard guard;
    if (!expect(guard.limit() == FrameFailureGuard::kDefaultLimit, "default limit")) {
        return 1;
    }
    for (std::uint32_t index = 1; index < guard.limit(); ++index) {
        if (!expect(!guard.failed(), "under the limit must not trip")) {
            return 1;
        }
        if (!expect(guard.consecutive() == index, "consecutive counts attempts")) {
            return 1;
        }
    }
    if (!expect(guard.failed(), "the last attempt trips the guard")) {
        return 1;
    }
    if (!expect(guard.failed(), "stays tripped while failures continue")) {
        return 1;
    }

    // A produced frame resets the run: failures must be consecutive, not cumulative.
    guard.succeeded();
    if (!expect(guard.consecutive() == 0, "success resets the counter")) {
        return 1;
    }
    for (std::uint32_t index = 1; index < guard.limit(); ++index) {
        if (!expect(!guard.failed(), "a reset guard starts a new run")) {
            return 1;
        }
    }

    // Explicit limits bound the run, including the degenerate zero limit (never render).
    FrameFailureGuard tight(3);
    if (!expect(!tight.failed() && !tight.failed(), "two failures under limit 3")) {
        return 1;
    }
    if (!expect(tight.failed(), "third failure trips limit 3")) {
        return 1;
    }
    FrameFailureGuard blocked(0);
    if (!expect(blocked.failed(), "zero limit trips immediately")) {
        return 1;
    }

    // Backpressure has its own, far larger budget: a busy slot is expected under load, so it must
    // outlive many more retries than the fault guard allows, but it must still be finite or a slot
    // that never completes would spin the UI thread forever.
    using tessera::app::BusyRetryBudget;
    BusyRetryBudget busy;
    if (!expect(busy.limit() == BusyRetryBudget::kDefaultLimit, "default busy budget")) {
        return 1;
    }
    if (!expect(busy.limit() > FrameFailureGuard::kDefaultLimit,
                "the busy budget outlasts the fault budget")) {
        return 1;
    }
    // Eight fault retries at the 2 ms backoff are ~16 ms, under one 60 Hz frame; the busy budget
    // must span several frames of real GPU backpressure.
    if (!expect(busy.limit() * 0.002 >= 0.25, "the busy budget spans at least a quarter second")) {
        return 1;
    }
    for (std::uint32_t index = 1; index < busy.limit(); ++index) {
        if (!expect(!busy.retried(), "under the busy budget must not give up")) {
            return 1;
        }
    }
    if (!expect(busy.retried(), "the last busy retry spends the budget")) {
        return 1;
    }
    if (!expect(busy.retried(), "a spent busy budget keeps reporting")) {
        return 1;
    }
    // A painted frame ends the run of busy retries, so ordinary backpressure is not cumulative.
    busy.succeeded();
    if (!expect(busy.consecutive() == 0, "a painted frame resets the busy budget")) {
        return 1;
    }
    BusyRetryBudget busy_blocked(0);
    if (!expect(busy_blocked.retried(), "a zero busy budget gives up immediately")) {
        return 1;
    }

    return 0;
}
