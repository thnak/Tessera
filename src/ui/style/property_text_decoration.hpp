// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/property_registry.hpp"

// The `text-decoration` shorthand the property registry delegates (docs/design/text-decoration.md).
namespace tessera::ui::style {

// `text-decoration: <line> || <style> || <color> || <thickness>`. Expands to the four longhands
// `text-decoration-line/-style/-color/-thickness`, resetting the ones the value omits to their
// initial values, as the CSS shorthand does. A style other than `solid`, a repeated component or
// any unknown component is reported as an unsupported value and the whole declaration is ignored,
// never half applied.
[[nodiscard]] PropertyExpansionResult
expand_text_decoration_shorthand(const Declaration& declaration);

} // namespace tessera::ui::style
