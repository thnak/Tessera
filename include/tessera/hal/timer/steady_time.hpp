// SPDX-License-Identifier: MIT

#pragma once

namespace tessera::hal {

// Monotonic seconds from a fixed epoch whose origin is arbitrary (typically boot).
// Values are not wall-clock time; callers may compare and subtract them but not format them.
[[nodiscard]] double steady_now_seconds() noexcept;

// Remaining time clamped to zero: max(0, deadline_seconds - now_seconds).
// A NaN comparison (either argument) yields zero rather than propagating NaN.
[[nodiscard]] double seconds_until(double now_seconds, double deadline_seconds) noexcept;

// Bounded idle wait until the deadline. Returns immediately when the deadline
// is already past and never spins. A non-finite remaining duration returns
// immediately so a bad deadline cannot park the UI thread indefinitely.
void wait_until_seconds(double deadline_seconds);

} // namespace tessera::hal
