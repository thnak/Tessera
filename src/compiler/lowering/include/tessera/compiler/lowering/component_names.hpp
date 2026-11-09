// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/lowering/name_resolver.hpp"
#include "tessera/compiler/rules/component_analysis.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::lowering {

// The slice 5 `NameResolver` seam implemented from the symbol table the analysis pass builds
// (docs/design/jsx-ir-lowering.md section 3). Lowering a hook initial value, a dependency or an
// inline handler argument needs the component's scope, so slice 6 supplies the names the analysis
// already resolved instead of asking the caller for a map.
//
// Resolved: a destructured prop (`prop::<name>`), a state variable (`state::<name>`), a ref
// (`ref::<name>`), an animated value (`anim::<name>`), a member of a non-destructured props
// parameter (`props.x` gives `prop::x`, decision 8), and names added with `add_local` (handler
// parameters are IR locals, docs/specs/ir-v1.md section 2.1).
//
// Not resolved, deliberately:
// - a local (`const x = pure_expr`): decision 8 inlines it, and that pass is not part of slice 6,
//   so `resolve` returns nullopt rather than inventing a `local::` reference the IR does not have;
// - a state setter or a handler name: they are function values, not expressions, and are looked up
//   through the handler records instead.
class ComponentNames final : public NameResolver {
public:
    explicit ComponentNames(const rules::ComponentAnalysis& analysis);

    // Adds a name that resolves to itself. Handler parameters are action locals, and they shadow
    // declarations of the same name.
    void add_local(std::string name);

    // Scope support for the loop variables of slice 8: `local_count()` is a mark, and
    // `truncate_locals(mark)` drops the names added since (the template of a `.map` loop sees its
    // item and index; the code after the loop does not).
    [[nodiscard]] std::size_t local_count() const {
        return locals_.size();
    }
    void truncate_locals(std::size_t count);

    [[nodiscard]] std::optional<std::string> resolve(std::string_view source_name) const override;

private:
    struct Entry {
        std::string name;
        std::string id;
    };

    std::vector<Entry> entries_;      // declarations, in analysis order
    std::vector<std::string> locals_; // checked first
    std::string props_param_;         // the non-destructured parameter, or empty
};

} // namespace tessera::compiler::lowering
