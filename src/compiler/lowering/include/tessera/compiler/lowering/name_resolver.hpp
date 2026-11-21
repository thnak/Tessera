// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace tessera::compiler::jsx {
struct Expr;
}

namespace tessera::compiler::lowering {

// Decision 8 of docs/design/jsx-ir-lowering.md: a `const` local has no IR id (`local::x` is never
// emitted). Lowering inlines it, so the resolver hands back the initializer to lower in place at
// each use instead of an id.
struct LocalBinding {
    // The initializer expression, owned by the AST, which outlives lowering.
    const jsx::Expr* initializer = nullptr;
    // An object-pattern binding (`const { state } = io;`) names one field of the initializer, so
    // lowering appends `.state` to the reference the initializer lowers to. Empty for
    // `const x = e;`.
    std::string field;
};

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

    // The initializer of the `const` local named `source_name`, or nullopt when the name is not a
    // local (or is shadowed by a handler parameter or a loop variable). A local is never a `ref`
    // (decision 8), so `resolve` returns nullopt for it and this is the only way to read one.
    [[nodiscard]] virtual std::optional<LocalBinding> local_binding(std::string_view) const {
        return std::nullopt;
    }
};

} // namespace tessera::compiler::lowering
