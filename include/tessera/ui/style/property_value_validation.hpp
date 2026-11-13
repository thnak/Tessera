// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/parser.hpp"

#include <string_view>
#include <vector>

namespace tessera::ui::style {

// Value validation for a recognised longhand. Reports the value forms CSS allows but V1 does not
// implement, and returns false when the whole declaration must be dropped because the property is
// a closed keyword set whose invalid value must not reach a runtime consumer
// (docs/specs/stylesheet-v1.md section 11: unsupported declarations are never silent).
//
// A value containing `var()` is accepted: substitution happens at computed-value time, after this
// check, so the pre-substitution token stream cannot be judged here. `property` is the normalized
// longhand name and `declaration` supplies the source offset for the diagnostic.
[[nodiscard]] bool validate_property_value(const Declaration& declaration,
                                           std::string_view property,
                                           std::vector<StyleDiagnostic>& out);

} // namespace tessera::ui::style
