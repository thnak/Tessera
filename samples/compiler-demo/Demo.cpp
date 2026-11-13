// SPDX-License-Identifier: MIT

#include "Demo.hpp"

namespace demo {

void Demo::sync() {
    ++sync_runs_;
    // Keep the demo counter non-negative after a dependency change.
    if (count() < 0) {
        set_count(0);
    }
}

void Demo::sync_cleanup() {
    // The demo owns nothing, so the cleanup body is intentionally empty.
}

} // namespace demo
