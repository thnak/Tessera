// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/validate.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tessera::compiler::ir {

// State shared by the validator passes. The sets are only used for lookup, never iterated, so
// the order of diagnostics comes from the order of traversal.
struct ValidationContext {
    const IrModule& module;
    std::vector<IrDiagnostic> diagnostics;
    std::unordered_set<std::string> component_ids;
    std::unordered_map<std::string, const Component*> components; // id -> record
    std::unordered_map<std::string, const Binding*> bindings;
    std::unordered_set<std::string> used_bindings; // `bind::` ids referenced by some use
    std::unordered_set<std::string> ui_ids;        // project-wide
    std::unordered_set<std::string> component_numeric_ids;

    void report(std::string code, std::string message, const SourceSpan& span);
};

// Names visible inside one component.
struct ComponentScope {
    const Component* component = nullptr;
    std::unordered_set<std::string> props; // names
    std::unordered_set<std::string> ids;   // state::, ref::, anim::, env:: ids
    std::unordered_set<std::string> handlers;
    std::unordered_set<std::string> node_ids;
    // Section 3.4: the component declares the `children` prop, so it may place a `Slot`.
    bool declares_children = false;
};

// Local names in scope (loop item and index, handler parameters).
using LocalNames = std::vector<std::string>;

// Pure lookups, no diagnostics.
ComponentScope make_scope(const Component& component);
bool reference_resolves(const ComponentScope& scope, const LocalNames& locals, std::string_view id);

void validate_expr(ValidationContext& context, const ComponentScope& scope,
                   const LocalNames& locals, const Expr* expr, const SourceSpan& where);
// An expression that must exist.
void require_expr(ValidationContext& context, const ComponentScope& scope, const LocalNames& locals,
                  const Expr* expr, const SourceSpan& where, const std::string& what);
void validate_actions(ValidationContext& context, const ComponentScope& scope,
                      const LocalNames& locals, const std::vector<Action>& actions,
                      const SourceSpan& where);
void validate_type(ValidationContext& context, const std::string& type, const SourceSpan& where);
void validate_span(ValidationContext& context, const SourceSpan& span, const std::string& what);
void validate_node_tree(ValidationContext& context, ComponentScope& scope, const Node& root);
void validate_component(ValidationContext& context, const Component& component);

} // namespace tessera::compiler::ir
