// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace tessera::compiler::lowering {

// Resolves a source reference chain to an IR reference id (docs/specs/ir-v1.md section 4).
//
// Slice 5 has no type table and no symbol table of its own, so lowering only defines this seam.
// The caller (today a test, later the analysis result of slice 3 plus the types of slice 4)
// receives the chain exactly as written, for example `count`, `props.title` or `item.row.id`, and
// returns the IR id (`state::count`, `prop::title`, `item.row.id`) or std::nullopt to reject it.
//
// The returned id is used verbatim. The IR validator checks it against the component scope
// (docs/specs/ir-v1.md section 9); lowering does not duplicate that check. A null resolver in
// ExpressionLoweringContext rejects every reference, which is the default.
class NameResolver {
public:
    virtual ~NameResolver() = default;

    [[nodiscard]] virtual std::optional<std::string>
    resolve(std::string_view source_name) const = 0;
};

} // namespace tessera::compiler::lowering
