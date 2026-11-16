// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/safe_area.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

// The **node half** of SafeArea (docs/specs/safe-area-v1.md sections 3 and 11; issue #223): how a
// library `SafeArea` instance reaches the inset arithmetic, and how the resolved insets reach the
// layout. It lives in the style module rather than in `src/app` because it is exactly the part a
// second layout consumer must share:
//
//   * `app::AppHost` — the production frame loop, whose `build_layout_input()` calls
//     `apply_safe_area_style()` on every node it lays out;
//   * the design-time preview (`tools/codegen/scene_prepare.cpp`) — issue #408's Wasm viewer, which
//     now feeds a simulated `FoldInfo` through `hal::resolve_safe_area()` and needs the same
//     token parsing and the same padding writer, or a foldable would preview differently from the
//     app.
//
// That sharing is the point rather than a convenience: a parallel "preview-only" resolution of the
// class tokens or a second padding writer is precisely the drift the issue's "no parallel layout
// path" rule forbids. Both callers own their own `WindowMetrics` (the app from the OS, the preview
// from a simulated posture) and both reach `domain` arithmetic through `hal::resolve_safe_area()`.
//
// A `Node` has no room for a new member and the JSX subset has no custom-property `style` keys, so
// the component's props travel as class tokens and the host writes the numbers.
namespace tessera::ui::style {

// The spec of a SafeArea node, read from its class tokens:
//   safe-edges-<a>+<b>    `top right bottom left start end` (default: all four)
//   safe-sources-<a>+<b>  `system-bars cutout ime gestures hinge` (default: all but gestures)
//   safe-min-<n>          a floor of n px per applying side (default 0)
//   safe-mode-margin      `margin` mode; absent means `padding`
// `rtl` comes from the node's computed `direction`. Allocation-free.
struct SafeAreaSpec {
    hal::SafeAreaConfig config;
    bool margin = false;
};

// True when `node` carries the `tsafe` token, i.e. it is a library `SafeArea` instance host.
[[nodiscard]] bool is_safe_area_node(const ui::Node& node);

// The spec of `node`, or `nullopt` when it is not a SafeArea node. A token this revision does not
// recognize contributes nothing (an unknown `sources` word is not a source), so a newer component
// cannot break an older host.
[[nodiscard]] std::optional<SafeAreaSpec> read_safe_area_spec(const ui::Node& node, bool rtl);

// Writes `insets` as the spec's `padding-*` or `margin-*` pixel declarations, replacing any
// physical or logical value the node had on those sides.
void apply_safe_area(ComputedStyle& style, const SafeAreaSpec& spec,
                     const hal::WindowInsets& insets);

// The one call both layout consumers make, in the form they both need it:
//   if `styles` is a SafeArea node's computed style and `metrics` resolved within the node, replace
//   its padding/margin sides with the resolved insets and return true.
//
// `rtl` comes from the node's own computed `direction` (the spec is logical-start/end aware),
// which is why the style is read here rather than passed in. A node that is not a SafeArea, or a
// caller with no metrics yet (`metrics == nullptr`), is left untouched: before the first snapshot
// arrives the whole window is usable, which is the app's documented pre-metrics behaviour.
//
// Allocation-free apart from the four `std::string`s `ComputedStyle::set` already owns, and it
// allocates nothing at all when the node carries no spec.
[[nodiscard]] bool apply_safe_area_style(ComputedStyle& style, const ui::Node& node,
                                         const hal::WindowMetrics* metrics);

} // namespace tessera::ui::style
