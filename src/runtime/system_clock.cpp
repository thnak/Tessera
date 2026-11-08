// SPDX-License-Identifier: MIT

#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/runtime/system_clock.hpp"

namespace tessera::runtime {

double SystemClock::now_seconds() const {
    return tessera::hal::steady_now_seconds();
}

} // namespace tessera::runtime
