// SPDX-License-Identifier: MIT

// Shared tables of the schema library (docs/specs/jsx-intrinsics-v1/machine-readable.md). Not
// installed: the data files implement these and the public lookups consume them.

#pragma once

#include "tessera/schema/intrinsics.hpp"
#include "tessera/schema/style.hpp"

#include <span>

namespace tessera::schema::internal {

[[nodiscard]] std::span<const TypeRecord> type_record_table();
[[nodiscard]] std::span<const PropRow> common_prop_table();
[[nodiscard]] std::span<const AliasAttr> common_alias_attr_table();
[[nodiscard]] std::span<const std::string_view> role_token_table();
[[nodiscard]] std::span<const ElementRow> element_table();
[[nodiscard]] std::span<const AliasRow> alias_table();
[[nodiscard]] std::span<const DomType> dom_type_table();
[[nodiscard]] std::span<const StyleProperty> style_property_table();

} // namespace tessera::schema::internal
