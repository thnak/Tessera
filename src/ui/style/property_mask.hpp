// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/property_registry.hpp"

namespace tessera::ui::style {

// `mask: <mask-layer>#` (issue #617, CSS Masking 1 section 6.9). A layer is an image
// (`none` or a gradient), `<position> [/ <size>]`, a repeat style and a mode, in any order. Each
// layer's parts land in `mask-image`, `mask-position`, `mask-size`, `mask-repeat` and `mask-mode`
// (CSS order, first layer on top), and every longhand is emitted, so the shorthand resets the ones
// a layer does not name. A geometry box (`mask-origin`/`mask-clip`) or a compositing operator
// (`mask-composite`) is reported and the declaration is ignored whole, never half applied.
[[nodiscard]] PropertyExpansionResult expand_mask_shorthand(const Declaration& declaration);

} // namespace tessera::ui::style
