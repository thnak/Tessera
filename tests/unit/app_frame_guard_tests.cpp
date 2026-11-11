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

    return 0;
}
