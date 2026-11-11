// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/frame_pacer.hpp"

namespace tessera::app {

// Bounds for one idle iteration of the window driver (docs/design/game-loop.md section 9).
//
// The driver never sleeps for a fixed amount: it parks on the bounded platform wait for the
// pacer's next deadline. The clamp matters because an uncapped pacer has no future deadline (a
// present is due immediately) and a settled scene still has to park instead of polling; the bound
// also caps how late a frame request or an input event can be noticed after a long park.
inline constexpr double kMaxIdleWaitSeconds = 0.25;

// Backoff after a frame could not be produced this iteration (both slots in flight, no swapchain
// image, a surface that has to be recreated). The slot may free on any GPU completion, so the
// retry is short and bounded; it is a retry yield, not the idle policy.
inline constexpr double kFrameRetryWaitSeconds = 0.002;

// Seconds until the pacer's next present deadline, clamped to [0, max_wait]. Returns 0 when the
// pacer says a frame is due now (including uncapped pacing) or schedules no positive wait, so a
// caller can never sleep past a deadline. A non-finite or non-positive `max_wait` degrades to 0.
[[nodiscard]] constexpr double pacer_wait_seconds(const runtime::PaceDecision& decision,
                                                  double max_wait = kMaxIdleWaitSeconds) noexcept {
    const double cap = max_wait > 0.0 ? max_wait : 0.0;
    if (decision.render_now || !(decision.wait_seconds > 0.0)) {
        return 0.0;
    }
    return decision.wait_seconds < cap ? decision.wait_seconds : cap;
}

// Idle budget for an iteration that has no frame to present (a settled scene, or a host frame the
// damage plan did not make due): the pacer deadline when one is scheduled, otherwise the full
// clamp so a static scene really parks and wakes on input/timer/completion events.
[[nodiscard]] constexpr double idle_wait_seconds(const runtime::PaceDecision& decision,
                                                 double max_wait = kMaxIdleWaitSeconds) noexcept {
    const double wait = pacer_wait_seconds(decision, max_wait);
    if (wait > 0.0) {
        return wait;
    }
    return max_wait > 0.0 ? max_wait : 0.0;
}

// True when a produced host frame must be painted and presented. A frame the runtime requested but
// that changed nothing (a wake with no damage) is skipped so it submits no GPU work; `force` is
// the bounded CI path (`run(max_frames > 0)`), which paints even a settled scene.
[[nodiscard]] constexpr bool should_present(bool frame_due, bool has_damage, bool force) noexcept {
    return force || (frame_due && has_damage);
}

} // namespace tessera::app
