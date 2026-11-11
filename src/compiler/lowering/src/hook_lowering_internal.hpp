// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/lowering/component_names.hpp"
#include "tessera/compiler/lowering/hook_lowering.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Internal seam of slice 6: the first-error state shared by the parts of hook lowering. Not
// installed; only src/compiler/lowering includes it.
namespace tessera::compiler::lowering::internal {

struct HookState {
    const HookLoweringContext& context;
    std::optional<jsx::Diagnostic> error; // the first error; later reports are ignored
};

// Reports a TESSERA_IR_* error at `span` unless one was already reported. The diagnostic keeps a
// string_view code, so `code` must point at a string literal.
void fail_hook(HookState& state, std::string_view code, std::string_view message, jsx::Span span,
               std::string_view suggestion = {});

// The callee of a call when it is a bare identifier (`useState`), otherwise empty.
std::string callee_name(const jsx::Expr& call);

// IR type of a written type annotation (docs/specs/jsx-subset-v1.md section 5). This is a
// syntactic copy, not the type table of slice 4: `int`, `float`, `string`, `boolean`, `T[]` and
// `T | null` have IR spellings, and any other name is copied verbatim as a record type name
// (docs/specs/ir-v1.md section 2 allows that until the type table lands). Nullopt when the form
// has no IR spelling yet (an inline object, a function type, a generic other than an array).
[[nodiscard]] std::optional<std::string> lower_written_type(const jsx::TypeNode& type);

// A handler return type annotation; `void` is the empty string (ir-v1.md section 6: absent means
// void), and a null annotation is empty too.
[[nodiscard]] std::optional<std::string> lower_return_type(const jsx::TypePtr& type);

// IR type of a `useState` initial value when `<T>` is omitted: the literal types of
// jsx-subset-v1.md section 5 and nothing else. Nullopt for a `null` literal (no IR null type) or
// any expression that needs the type table.
[[nodiscard]] std::optional<std::string> initial_literal_type(const jsx::Expr& expr);

// Lowers one component expression with the component's names, or null with the first error
// recorded in `state`.
[[nodiscard]] ir::ExprPtr lower_component_expression(HookState& state, const jsx::Expr& expr,
                                                     const NameResolver& names);

// The actions of a setter-call arrow (jsx-subset-v1.md section 9.2). An empty arrow is an empty
// list; a statement that is not a setter call or an animated-value `.set(...)` is an error.
[[nodiscard]] std::vector<ir::Action> lower_actions(HookState& state, const jsx::Expr& arrow);

// The preview body of a native handler (jsx-subset-v1.md section 9.3): setter calls and animated
// `.set(...)` calls as actions, plus an optional final `return <PureExpr>;`. Nullopt when the body
// is empty or is outside those rules; the front end does not yet diagnose such a body, so the
// designer has nothing to run for it, and no error is recorded in `state`.
[[nodiscard]] std::optional<ir::Preview> lower_preview(HookState& state, const jsx::Expr& arrow);

// The parameters of a declared handler from its annotations. A parameter without a written type
// is an error: event payload types need the type table of slice 4.
[[nodiscard]] std::vector<ir::Param> lower_handler_params(HookState& state, const jsx::Expr& arrow);

// One hook or binding of a component, in source order. `lower_hooks` calls these from its single
// body pass; each reports at most one error in `state`.
void lower_effect_hook(HookState& state, std::size_t index, LoweredHooks& out);
void lower_declared_arrow_hook(HookState& state, std::size_t index, LoweredHooks& out);

} // namespace tessera::compiler::lowering::internal
