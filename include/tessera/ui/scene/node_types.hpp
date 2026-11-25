// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

// The value vocabulary of `ui::Node`: its id type and the small enums and records its setters take.
// They live apart from the class (docs/architecture/god-type-audit-v1.md section 3.1) so a header
// or translation unit that only names a `UserSelect`, a `ScrollBehavior` or a `NodeId` does not
// pull the whole node declaration, and so adding an enumerator here recompiles the files that read
// it instead of every includer of `node.hpp`. `Node` re-exports each name (`Node::PaintOutsets`,
// `Node::FocusGroup`), which generated code and tests spell.
//
// Nothing here adds a member to `Node`; the layout and `kNodeSizeBudget` are unaffected.
namespace tessera::ui {

using NodeId = std::uint64_t;

// Text-selection policy from `user-select` (stylesheet-v1 section 11, issue #247). `None` blocks
// starting a selection at this node, `Text` allows it, and `Auto` keeps the default (static text is
// selectable). The property is inherited, so each node stores its computed value and one explicit
// `auto`/`text` anywhere in a `none` subtree re-enables selection there.
enum class UserSelect : std::uint8_t {
    Auto,
    None,
    Text,
    // `all`: a press selects the whole element's text instead of placing a caret (stylesheet-v1
    // section 11, issue #279).
    All,
};

// Scroll runtime style inputs (docs/specs/stylesheet-v1.md section 11 and
// docs/specs/scroll-and-gestures-v1.md section 4). The values mirror the CSS keywords the registry
// accepts; the scene stores the resolved enum so the scroll controller and paint never parse CSS.
// `overscroll-effect` uses the names of docs/specs/animation-v1.md section 7 and has no `auto`
// keyword: `Platform` is the initial request the HAL profile resolves.
//
// `Node` packs the three values and the two scrollable-axis bits into one byte, and that byte is
// what lets the ten scroll floats fit inside `kNodeSizeBudget`, so the packing stays on `Node`.
enum class ScrollBehavior : std::uint8_t {
    Auto,
    Smooth,
};
enum class OverscrollBehavior : std::uint8_t {
    Auto,
    Contain,
    None,
};
enum class OverscrollEffect : std::uint8_t {
    None,
    Glow,
    Stretch,
    Bounce,
    Platform,
};

// JSX `focusTrap` and `focusGroup` (issue #236): a `focus_group` node is a roving-focus group whose
// focusable descendants form one Tab stop.
enum class FocusGroup : std::uint8_t { None, Horizontal, Vertical, Both };

// How far a node paints beyond its border box (CSS box-shadow reach), in box-local px. Damage uses
// it so a node's old and new pixels, including its shadow, are redrawn. Negative and non-finite
// values count as 0.
struct PaintOutsets {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    // True when an outer shadow has a blur radius: the one effect adaptive quality can reduce
    // (performance.md section 6.1). Sharp shadows reach outward too but have nothing to reduce.
    bool blurred = false;
};

} // namespace tessera::ui
