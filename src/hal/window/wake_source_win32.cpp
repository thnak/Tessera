// SPDX-License-Identifier: MIT
//
// The Win32 wake primitive (issue #842): an event a parked `MsgWaitForMultipleObjectsEx` joins
// beside the high-resolution timer and the message queue. A completion posted from a worker sets
// it, the park returns, and the next tick drains the queue on the UI thread. Nothing here runs a
// completion.
//
// The event is manual-reset so that a wait observes the signal without consuming it, the same
// contract as the POSIX eventfd: `drain()` is the only thing that clears it, and a burst of wakes
// before the UI thread drains is one signalled event, which matches the completion queue's one
// wake per empty -> non-empty edge.

#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/hal/wake_source.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace tessera::hal {

namespace {

class Win32WakeSource final : public WakeSource {
public:
    explicit Win32WakeSource(HANDLE event) noexcept : event_(event) {}
    Win32WakeSource(const Win32WakeSource&) = delete;
    Win32WakeSource& operator=(const Win32WakeSource&) = delete;
    ~Win32WakeSource() override {
        CloseHandle(event_);
    }

    void wake() noexcept override {
        SetEvent(event_);
    }

    void drain() noexcept override {
        ResetEvent(event_);
    }

    [[nodiscard]] bool wait_until(double deadline_seconds) override {
        const double remaining = deadline_seconds - steady_now_seconds();
        // A past or non-finite deadline is a poll: report whether the wake is already signalled.
        double wait_milliseconds = 0.0;
        if (std::isfinite(remaining) && remaining > 0.0) {
            wait_milliseconds = std::min(std::ceil(remaining * 1000.0),
                                         static_cast<double>(std::numeric_limits<DWORD>::max() - 1));
        }
        return WaitForSingleObject(event_, static_cast<DWORD>(wait_milliseconds)) == WAIT_OBJECT_0;
    }

    [[nodiscard]] void* os_event() const noexcept override {
        return event_;
    }

private:
    HANDLE event_;
};

} // namespace

std::unique_ptr<WakeSource> create_win32_wake_source() {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        return nullptr;
    }
    return std::make_unique<Win32WakeSource>(event);
}

} // namespace tessera::hal
