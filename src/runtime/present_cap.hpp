// SPDX-License-Identifier: MIT
//
// The adaptive present cap (docs/design/performance.md section 6, step 3;
// docs/specs/runtime-config-v1.md sections 5.2 and 5.7).
//
// One pure function: (display refresh, configured cap, quality step) -> present cap in Hz. It is
// deliberately separate from the controller's hysteresis state machine: the controller owns *when*
// quality moves, this owns *what the cap is* at a step, so the rule is testable without a window, a
// pacer or a clock. The pacer consumes the result through its existing rate-change path
// (FramePacer::update), which is also what a scripted refresh change uses.

#pragma once

#include <cstdint>

namespace tessera::runtime {

// The documented floor of the adaptive present cap. Below ~10 Hz an interactive UI stops being
// interactive; the design order's next lever (step 4, pause non-essential animations) is the right
// answer there, not a slower present rate.
inline constexpr double kAdaptivePresentCapFloorHz = 10.0;

// The present cap for one quality step.
//
//   step 0        -> `configured_cap_hz` unchanged: full quality never moves the cap.
//   refresh <= 0  -> `configured_cap_hz` unchanged: no divisor exists, so the pacer keeps its
//                    documented deadline pacing (runtime-config-v1.md section 5.2).
//   otherwise     -> the next refresh divisor below the previous step's effective rate
//                    (refresh/2, refresh/3, ...), clamped to kAdaptivePresentCapFloorHz.
//
// The effective rate at step 0 is min(configured_cap_hz, refresh) when the cap is non-zero, else
// the refresh itself, so a step never raises the rate above `configured_cap_hz` (the `maxFps`
// ceiling) or the refresh rate. A cap of 0 means uncapped and stays 0 when the refresh is unknown.
[[nodiscard]] double adaptive_present_cap_hz(double refresh_hz, double configured_cap_hz,
                                             std::uint32_t quality_step) noexcept;

} // namespace tessera::runtime
