// SPDX-License-Identifier: MIT

#pragma once

#include "color_functions.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <vector>

namespace tessera::ui::style::detail {

// The `light-dark()`/`color-mix()` half of computed-value-time substitution, split from
// `var_resolve.cpp` (which resolves `var()` and `env()`) to keep both under the file limit.
// `resolve_variables` runs this after the custom-property pass.
[[nodiscard]] ColorContext color_context_for(const ComputedStyle& style,
                                             const ComputedStyle* parent);
void resolve_color_functions(ComputedStyle& style, const ComputedStyle* parent,
                             const ColorContext& context,
                             std::vector<StyleDiagnostic>* diagnostics);

} // namespace tessera::ui::style::detail
