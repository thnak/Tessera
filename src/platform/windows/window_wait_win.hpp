// SPDX-License-Identifier: MIT
//
// The Win32 idle park of Window::wait_for_events: one bounded wait on the high-resolution timer,
// the host's wake event and the message queue. Split from window.cpp, which is over the
// file-size limit and must only shrink.

#pragma once

#include "tessera/hal/wake_source.hpp"

#include <cstdint>
#include <windows.h>

namespace tessera::hal::detail {

// Why one park ended.
enum class ParkOutcome : std::uint8_t {
    Wake,    // the host's completion wake fired
    Timeout, // the remaining budget (or the timer) elapsed
    Input,   // a message is queued; the caller drains it and re-checks
    Failed,  // the wait itself failed; the caller idles out the budget once
};

// Owns the lazily created waitable timer. Window thread only.
class Win32Parker {
public:
    Win32Parker() = default;
    Win32Parker(const Win32Parker&) = delete;
    Win32Parker& operator=(const Win32Parker&) = delete;
    ~Win32Parker();

    // Parks for at most `remaining_seconds` (> 0). `wake` may be null; it is only read here, so
    // the caller drains it on `ParkOutcome::Wake`.
    [[nodiscard]] ParkOutcome park(double remaining_seconds, const WakeSource* wake);

private:
    // Null when the OS has no high-resolution timers.
    [[nodiscard]] HANDLE timer();

    HANDLE timer_ = nullptr;
    bool timer_failed_ = false;
};

} // namespace tessera::hal::detail
