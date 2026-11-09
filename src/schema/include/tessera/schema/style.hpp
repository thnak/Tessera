// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/schema/intrinsics.hpp"

#include <span>
#include <string_view>

// The style half of the shared schema (docs/specs/jsx-intrinsics-v1/machine-readable.md section 5):
// one table of supported CSS property names and their value kind. The compiler uses the kind for
// the inline `style` number rule of docs/design/jsx-ir-lowering.md decision 10; the runtime
// property registry keeps initial values and inheritance and checks its names against this table.
//
// The rows are the longhands of src/ui/style/property_registry.cpp; shorthands are expanded by the
// registry and are not rows here.
namespace tessera::schema {

struct StyleProperty {
    std::string_view name; // kebab-case CSS name
    StyleKind kind = StyleKind::Keyword;
    std::string_view note;
};

[[nodiscard]] std::span<const StyleProperty> style_properties();

// Null when the property is not in the table.
[[nodiscard]] const StyleProperty* find_style_property(std::string_view name);

} // namespace tessera::schema
