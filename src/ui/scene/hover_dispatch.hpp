// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"
#include "tessera/ui/scene/node.hpp"

namespace tessera::ui {

// Fires the W3C-style boundary events for a hover target change; either side may be null (null
// means "no hovered node"). PointerOut/PointerOver bubble from their target down to (excluding)
// the deepest common ancestor of the two chains; PointerLeave/PointerEnter are non-bubbling and
// fire once per node strictly below that ancestor (leave innermost -> outermost, enter outermost
// -> innermost). Capture suppression and the touch filter are the caller's responsibility. The
// rules are documented in docs/design/input.md section 7.
void dispatch_hover_transition(Node* old_target, Node* new_target, const InputEvent& input);

} // namespace tessera::ui
