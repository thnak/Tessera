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
// parameter (`props.x` gives `prop::x`, decision 8), a behavior output (`press.pressed` gives
// `beh::press.pressed`, behavior-hooks-v1.md section 6), and names added with `add_local` (handler
// parameters are IR locals, docs/specs/ir-v1.md section 2.1).
//
// A `const` local (`const x = pure_expr`) is not a reference: decision 8 inlines it, so `resolve`
// returns nullopt for it and `local_binding` hands back the initializer to lower in place at each
// use. That keeps `local::x` out of the IR, which has no such id.
//
// Not resolved, deliberately: a state setter or a handler name. They are function values, not
// expressions, and are looked up through the handler records instead.
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

    // Decision 8: the initializer of the `const` local `source_name`, for lowering to inline. A
    // dotted chain whose base is a local (`row.title` for `const row = props.row;`) returns the
    // base's initializer with `field` = `title`, so the caller appends it to the lowered ref. A
    // name shadowed by a handler parameter or a loop variable is not a local here: `resolve`
    // answers it first.
    [[nodiscard]] std::optional<LocalBinding>
    local_binding(std::string_view source_name) const override;

private:
    struct Entry {
        std::string name;
        std::string id;
    };

    // One `const` local of the component body: the name it introduces, the initializer to inline,
    // and the field an object pattern selected (`const { state } = io;` gives `io` and `state`).
    struct ConstLocal {
        std::string name;
        const jsx::Expr* initializer = nullptr;
        std::string field;
    };

    // Records the local `name` of `statement` in `const_locals_`, if the statement gives it an
    // initializer to inline.
    void record_const_local(const jsx::Statement& statement, std::string_view name);

    std::vector<Entry> entries_;      // declarations, in analysis order
    std::vector<std::string> locals_; // checked first
    std::string props_param_;         // the non-destructured parameter, or empty
    // One entry per behavior hook call that returns outputs: the local name the call was bound to
    // (`press`) and the output names it returns, in manifest order. A reference `press.pressed`
    // resolves to `beh::press.pressed` (behavior-hooks-v1.md section 6). A bare-statement call has
    // no local name, so nothing can reference its outputs and it is not recorded here.
    struct BehaviorOutputs {
        std::string name;
        std::vector<std::string> outputs;
    };
    std::vector<BehaviorOutputs> behavior_outputs_;
    // Decision 8: the `const` locals of the body, in declaration order. A local is inlined, never
    // referenced, so it is kept apart from `entries_`.
    std::vector<ConstLocal> const_locals_;
};

} // namespace tessera::compiler::lowering
