// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/property_registry.hpp"

// The `font-variant` shorthand the property registry delegates (docs/design/text.md section 3.1).
namespace tessera::ui::style {

// `font-variant: normal | none | <keyword>+`. Each keyword belongs to one longhand group
// (ligatures, numeric, caps, east-asian, position); the expansion writes all five longhands,
// resetting the groups the value omits to `normal`, as the CSS shorthand does. `none` means
// `font-variant-ligatures: none` with the rest `normal`. An unknown keyword, a conflicting pair
// or a repeated group reports an unsupported value and the whole declaration is ignored.
[[nodiscard]] PropertyExpansionResult expand_font_variant_shorthand(const Declaration& declaration);

} // namespace tessera::ui::style
