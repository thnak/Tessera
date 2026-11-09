// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/paint_data.hpp"

namespace tessera::ui::text {

// Caret blink timing (docs/specs/text-editing-v1.md section 12). The state machine is driven by the
// fixed-step loop, never by paint: the owner calls `tick(now_seconds)` once per tick with a
// monotonic time source, and only the fixed-step layer decides whether a frame is due.
//
// Phases: visible for `config.visible_seconds`, then hidden for `config.hidden_seconds`, repeating.
// The default is 500/500 ms, like platform text editors. The visible window is half-open
// `[0, visible_seconds)` and the hidden window is `[visible_seconds, period)`, so a tick at exactly
// the visible boundary reports hidden and a tick at exactly the period boundary reports visible
// again. A tick that jumps over several phases flips at most once (it reports the change from the
// previous state); a caller that needs every transition ticks at least once per phase.
//
// Rules:
// - Reset on activity: typing, a caret move, or a selection change calls `reset(now)`, which shows
//   the caret immediately (when the field is active) and restarts the cycle at `now`.
// - A non-collapsed selection hides the caret (platform-typical: Windows/macOS/Linux editors hide
//   the caret while a range is selected), and an unfocused or read-only field hides it too. The
//   owner expresses both through `set_active(...)`; `EditController::caret_blink_active()` computes
//   the focus/read-only/selection policy in one place.
// - Repaint policy: `tick()`/`reset()`/`set_active()` return true only when the effective
//   visibility changed, i.e. exactly two transitions per period and no flags in between. The owner
//   publishes the result into `PaintData::decoration` (see `publish_caret_visibility`) and marks
//   paint dirty only when the flag is true.
struct CaretBlinkConfig {
    double visible_seconds = 0.5;
    double hidden_seconds = 0.5;
};

class CaretBlink {
public:
    // Both phases must be finite and > 0; throws std::invalid_argument otherwise.
    explicit CaretBlink(CaretBlinkConfig config = {});

    // Advances the state machine to `now_seconds`. Time must be monotonic; a backwards timestamp
    // is clamped to the previous one (deterministic, never rewinds a phase). Returns true when the
    // effective visibility changed, i.e. a repaint is needed for the caret.
    bool tick(double now_seconds);

    // Activity (typing, caret move, selection change): show the caret immediately when active and
    // restart the cycle at `now_seconds`. Returns true when the caret became
    // visible/repaint-worthy.
    bool reset(double now_seconds);

    // Field activity: only a focused, editable field with a collapsed selection blinks. Turning it
    // off hides the caret immediately; turning it on shows the caret immediately and restarts the
    // cycle. Returns true when the effective visibility changed.
    bool set_active(bool active, double now_seconds);

    [[nodiscard]] bool caret_visible() const {
        return active_ && visible_;
    }
    [[nodiscard]] bool active() const {
        return active_;
    }
    [[nodiscard]] double now() const {
        return now_;
    }
    [[nodiscard]] const CaretBlinkConfig& config() const {
        return config_;
    }

    // Monotonic timestamp of the next visible<->hidden transition, for on-demand frame scheduling.
    // Infinite while inactive (nothing to schedule). Values are exact phase-boundary times, not
    // "now + remaining": a tick may land exactly on the boundary.
    [[nodiscard]] double next_transition_seconds() const;

private:
    void clamp_time(double now_seconds);
    [[nodiscard]] bool phase_visible(double now_seconds) const;

    CaretBlinkConfig config_;
    double anchor_ = 0.0;
    double now_ = 0.0;
    bool active_ = true;
    bool visible_ = true;
};

// Publishes the blink result into the paint decoration the paint pass consumes. Paint stays
// timer-free: only the fixed-step owner advances CaretBlink and calls this; `caret_visible` and
// `blink_phase` are kept consistent with the paint rule
// (`decoration.caret_visible && decoration.blink_phase >= 0.5f`), so paint reads one coherent
// snapshot and never keeps its own clock.
//
// Damage note: only the caret's visibility changes, never the text layout. The current damage pass
// damages the whole text node, which is conservative for a caret-only change; a tighter caret-rect
// damage would need a paint-side caret rect, which is deferred (paint owns the decoration).
inline void publish_caret_visibility(TextDecoration& decoration, const CaretBlink& blink) {
    const bool visible = blink.caret_visible();
    decoration.caret_visible = visible;
    decoration.blink_phase = visible ? 1.0f : 0.0f;
}

} // namespace tessera::ui::text
