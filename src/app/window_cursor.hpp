// SPDX-License-Identifier: MIT
//
// App-side pointer-shape plumbing (issue #191, docs/specs/window-chrome-v1.md section 8.5).
//
// This is the middle of the three-layer chain: `ui::style` parses the CSS keyword into a
// `style::CursorKind`, the scene stores it on the node, and this layer maps that onto the HAL's
// `hal::CursorShape` and asks the platform window for it. The UI never includes a `hal::` header
// and the HAL never includes a UI header (ADR-020).
//
// Not a public header: the mapping and the one-shot unsupported diagnostic are the app driver's
// internals. `include/tessera/app/window.hpp` stays unchanged.

#pragma once

#include "tessera/hal/cursor.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/cursor_value.hpp"

namespace tessera::app::detail {

// Maps a resolved CSS cursor kind onto the HAL's native shape. One `switch` over every enumerator,
// so a kind added later is a compiler warning rather than a silent `default` mapping. `Auto` and
// `Default` both mean the platform arrow (the distinction is CSS syntax, not platform intent) and
// CSS `none` becomes `Hidden`.
[[nodiscard]] hal::CursorShape to_hal_cursor(ui::style::CursorKind kind) noexcept;

// The shape the hovered scene node asks for, or `Default` when nothing is hovered. `cursor` is
// inherited, so the node's value already carries the ancestor resolution.
[[nodiscard]] hal::CursorShape hovered_cursor(const ui::Node* hovered) noexcept;

// Requests `wanted` from `window` when it differs from `last` (a static hover therefore makes no
// platform call), and reports the unsupported platform exactly once through `unsupported_reported`.
// Returns true when this call printed the diagnostic, so the caller and a test can see the one-shot
// behaviour without scraping stderr. Never blocks and never throws: the platform applies the shape
// on its next event-loop iteration.
[[nodiscard]] bool apply_cursor(hal::Window& window, hal::CursorShape wanted,
                                hal::CursorShape& last, bool& unsupported_reported);

} // namespace tessera::app::detail
