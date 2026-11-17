// SPDX-License-Identifier: MIT

#pragma once

// The host's SafeArea state (docs/specs/safe-area-v1.md sections 4 and 6): the window metrics the
// insets come from, and the animated value of each inset edge. Private to src/app.
//
// The **node** half of SafeArea - which nodes are `SafeArea` instances, what their class tokens
// say, and how resolved insets become `padding-*`/`margin-*` - is
// `tessera/ui/style/safe_area_style.hpp` since issue #408. It moved there because the design-time
// preview is a second layout consumer and must resolve a `SafeArea` through the same code the app
// does. What remains here is the part a preview cannot have: an ease-out over successive frames,
// which needs a clock.

#include "tessera/hal/safe_area.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/ui/style/safe_area_style.hpp"

namespace tessera::app {

class SafeAreaState {
public:
    // Time constant of the ease-out used when the platform supplies no insets animation.
    static constexpr double kTimeConstantSeconds = 0.06;

    // Takes the newest metrics as the target. The first call, a size change (rotation) with
    // `animate == false`, and reduced motion apply the value at once; otherwise each edge of the
    // system bars, the keyboard and the gesture zones eases toward it independently. Cutout and
    // hinge rectangles are geometry and always apply at once.
    // Returns false when no input of the arithmetic changed, so a static window relayouts nothing.
    bool set_metrics(const hal::WindowMetrics& metrics, bool reduced_motion);

    // Advances every edge by `dt` seconds. Returns true when any animated value changed.
    bool step(double dt, bool reduced_motion);

    [[nodiscard]] bool animating() const noexcept {
        return animating_;
    }
    [[nodiscard]] bool has_metrics() const noexcept {
        return has_metrics_;
    }
    [[nodiscard]] const hal::WindowMetrics& current() const noexcept {
        return current_;
    }

    // The metrics a layout consumer resolves through, or nullptr before the first snapshot. The
    // shared `ui::style::apply_safe_area_style()` takes exactly this pointer, so the app and the
    // preview pass the same kind of value and neither invents a "no metrics yet" convention.
    [[nodiscard]] const hal::WindowMetrics* metrics_or_null() const noexcept {
        return has_metrics_ ? &current_ : nullptr;
    }

    // The resolved insets of one SafeArea node under the current (possibly mid-animation) values.
    [[nodiscard]] hal::WindowInsets resolve(const hal::SafeAreaConfig& config) const noexcept {
        return hal::resolve_safe_area(current_, config);
    }

private:
    hal::WindowMetrics current_;
    hal::WindowMetrics target_;
    bool has_metrics_ = false;
    bool animating_ = false;
};

} // namespace tessera::app
