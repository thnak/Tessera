// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/property_registry.hpp"

namespace tessera::ui::style {

// `mask: <mask-layer>#` (issue #617, CSS Masking 1 section 6.9). A layer is an image
// (`none` or a gradient), `<position> [/ <size>]`, a repeat style, a mode, one or two geometry
// boxes or `no-clip`, and a compositing operator, in any order. Each layer's parts land in the
// eight longhands (CSS order, first layer on top), and every longhand is emitted, so the shorthand
// resets the ones a layer does not name. `margin-box` is reported and the declaration is ignored
// whole, never half applied.
[[nodiscard]] PropertyExpansionResult expand_mask_shorthand(const Declaration& declaration);

} // namespace tessera::ui::style
