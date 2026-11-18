// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/property_registry.hpp"

#include <string_view>

// Value rules the property registry delegates so that file stays under the source-size limit:
// the `background` shorthand and the `window-backdrop` keyword set.
namespace tessera::ui::style {

// `background: <color> || <linear-gradient()>` (issue #457). The colour goes to `background-color`
// and the gradient to `background-image`; every other layer (`url()`, repeat, position, size,
// attachment, box) is reported as an unsupported value and the declaration is ignored whole, never
// half applied. A lone `none` resets both longhands.
[[nodiscard]] PropertyExpansionResult expand_background_shorthand(const Declaration& declaration);

// True when `text` is one keyword of the `window-backdrop` grammar (glass-v1.md section 2).
// `auto` is a valid request even though it is never a resolved material.
[[nodiscard]] bool is_window_backdrop_value(std::string_view text);

} // namespace tessera::ui::style
