// SPDX-License-Identifier: MIT

#pragma once

// The host's SafeArea state (docs/specs/safe-area-v1.md sections 4 and 6): the window metrics the
// insets come from, and the animated value of each inset edge. Private to src/app.

#include "tessera/hal/safe_area.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <optional>
#include <string>

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

// The spec of a SafeArea node, read from its class tokens (a `Node` has no room for a new member,
// and the JSX subset has no custom-property `style` keys). A node is a SafeArea when its class has
// the token `tsafe`; the library component adds the others:
//   safe-edges-<a>+<b>    `top right bottom left start end` (default: all four)
//   safe-sources-<a>+<b>  `system-bars cutout ime gestures hinge` (default: all but gestures)
//   safe-min-<n>          a floor of n px per applying side (default 0)
//   safe-mode-margin      `margin` mode; absent means `padding`
// `rtl` comes from the node's computed `direction`. Allocation-free.
struct SafeAreaSpec {
    hal::SafeAreaConfig config;
    bool margin = false;
};

[[nodiscard]] bool is_safe_area_node(const ui::Node& node);
[[nodiscard]] std::optional<SafeAreaSpec> read_safe_area_spec(const ui::Node& node, bool rtl);

// Writes the resolved insets of `spec` as `padding-*` or `margin-*` pixel declarations into a
// computed style, replacing any physical or logical value the node had on those sides.
void apply_safe_area(ui::style::ComputedStyle& style, const SafeAreaSpec& spec,
                     const hal::WindowInsets& insets);

} // namespace tessera::app
