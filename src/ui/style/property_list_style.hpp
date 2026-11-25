// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/property_registry.hpp"

// The `list-style` shorthand the property registry delegates (docs/design/list-style.md).
namespace tessera::ui::style {

// `list-style: <type> || <position>`, or `none`. Expands to `list-style-type` and
// `list-style-position`, resetting the one the value omits to its initial value (`disc`,
// `outside`), as the CSS shorthand does. `list-style-image` (`url()`), `inside`, a counter style
// the engine cannot draw, a repeated component or an unknown one is reported as an unsupported
// value and the whole declaration is ignored, never half applied.
[[nodiscard]] PropertyExpansionResult expand_list_style_shorthand(const Declaration& declaration);

} // namespace tessera::ui::style
