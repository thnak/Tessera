// SPDX-License-Identifier: MIT

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#include "window_wait_win.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace tessera::hal::detail {
namespace {

// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is missing from older SDK headers.
constexpr DWORD kHighResolutionTimer = 0x00000002;
constexpr DWORD kTimerBoundPaddingMs = 20;

} // namespace

Win32Parker::~Win32Parker() {
    if (timer_ != nullptr) {
        CloseHandle(timer_);
    }
}

HANDLE Win32Parker::timer() {
    if (timer_ == nullptr && !timer_failed_) {
        // The default sleep granularity is the 15.6 ms system tick, which turns a 16.7 ms frame
        // deadline into a ~31 ms wait. A high-resolution timer (Windows 10 1803+) wakes within
        // about a millisecond without changing the global timer resolution.
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, kHighResolutionTimer, TIMER_ALL_ACCESS);
        timer_failed_ = timer_ == nullptr;
    }
    return timer_;
}

ParkOutcome Win32Parker::park(double remaining_seconds, const WakeSource* wake) {
    // MsgWaitForMultipleObjectsEx takes whole milliseconds. Round up so a sub-millisecond
    // remainder still parks once instead of spinning, and clamp so the wait stays bounded.
    const double wait_milliseconds =
        std::min(std::ceil(remaining_seconds * 1000.0),
                 static_cast<double>(std::numeric_limits<DWORD>::max() - 1));
    const DWORD wait_ms = static_cast<DWORD>(wait_milliseconds);

    // Park on the high-resolution timer plus the message queue. The millisecond timeout stays as a
    // bound in case the timer never fires (it is padded so the timer normally wins).
    HANDLE timer_handle = timer();
    bool timer_armed = false;
    if (timer_handle != nullptr) {
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>(remaining_seconds * 1.0e7));
        timer_armed = SetWaitableTimer(timer_handle, &due, 0, nullptr, nullptr, FALSE) != FALSE;
    }
    // The completion wake joins the handle list (#842): a worker's post ends the park here instead
    // of waiting out the host's clamp. Handle order is timer, then wake.
    HANDLE handles[2];
    DWORD handle_count = 0;
    if (timer_armed) {
        handles[handle_count++] = timer_handle;
    }
    DWORD wake_index = WAIT_FAILED;
    if (wake != nullptr && wake->os_event() != nullptr) {
        wake_index = handle_count;
        handles[handle_count++] = static_cast<HANDLE>(wake->os_event());
    }
    const DWORD result = MsgWaitForMultipleObjectsEx(
        handle_count, handle_count > 0 ? handles : nullptr,
        timer_armed ? wait_ms + kTimerBoundPaddingMs : wait_ms, QS_ALLINPUT, 0);
    if (wake_index != WAIT_FAILED && result == WAIT_OBJECT_0 + wake_index) {
        return ParkOutcome::Wake;
    }
    if (result == WAIT_FAILED) {
        return ParkOutcome::Failed;
    }
    if (result == WAIT_TIMEOUT || (timer_armed && result == WAIT_OBJECT_0)) {
        return ParkOutcome::Timeout;
    }
    return ParkOutcome::Input;
}

} // namespace tessera::hal::detail
