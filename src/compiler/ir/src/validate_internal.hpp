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
    explicit ValidationContext(const IrModule& module_ref)
        : module(module_ref) {}

    const IrModule& module;
    std::vector<IrDiagnostic> diagnostics;
    std::unordered_set<std::string> component_ids;
    std::unordered_map<std::string, const Component*> components; // id -> record
    std::unordered_map<std::string, const Binding*> bindings;
    std::unordered_set<std::string> used_bindings; // `bind::` ids referenced by some use
    std::unordered_set<std::string> ui_ids;        // project-wide
    std::unordered_set<std::string> component_numeric_ids;
    // Section 2.2: the declared record types by name. A member type and a `record` value resolve
    // against this table; `used_types` records which declared records some declaration or value
    // names, so the warnings pass can report a dead one.
    std::unordered_map<std::string, const RecordType*> types;
    std::unordered_set<std::string> used_types;

    void report(std::string code, std::string message, const SourceSpan& span);
};

// Names visible inside one component.
struct ComponentScope {
    const Component* component = nullptr;
    std::unordered_set<std::string> props; // names
    std::unordered_set<std::string> ids;   // state::, ref::, anim::, env:: ids
    std::unordered_set<std::string> handlers;
    std::unordered_set<std::string> node_ids;
    // The declared IR type of every id a `ref` may name, keyed by the ref base form (`state::x`,
    // `prop::x`, `anim::x`, `env::x`). A dotted ref is member-checked against it (section 4.3).
    std::unordered_map<std::string, std::string> declared_types;
    // Section 3.4: the component declares the `children` prop, so it may place a `Slot`.
    bool declares_children = false;
};

// Local names in scope (loop item and index, handler parameters).
using LocalNames = std::vector<std::string>;

// Pure lookups, no diagnostics.
ComponentScope make_scope(const Component& component);
bool reference_resolves(const ComponentScope& scope, const LocalNames& locals, std::string_view id);

// `record_value_allowed` is true in the positions section 4.3 permits (a `useState` initial, a prop
// default, a field of another record value); everywhere else a `record` expression is rejected.
void validate_expr(ValidationContext& context, const ComponentScope& scope,
                   const LocalNames& locals, const Expr* expr, const SourceSpan& where,
                   bool record_value_allowed = false);
// An expression that must exist.
void require_expr(ValidationContext& context, const ComponentScope& scope, const LocalNames& locals,
                  const Expr* expr, const SourceSpan& where, const std::string& what,
                  bool record_value_allowed = false);
void validate_actions(ValidationContext& context, const ComponentScope& scope,
                      const LocalNames& locals, const std::vector<Action>& actions,
                      const SourceSpan& where);
void validate_type(ValidationContext& context, const std::string& type, const SourceSpan& where);
// The same check, with the record name optionally not marked as used (a member type of a record
// that is itself unused does not make the member's record used).
void validate_type(ValidationContext& context, const std::string& type, const SourceSpan& where,
                   bool mark_used);
void validate_span(ValidationContext& context, const SourceSpan& span, const std::string& what);
void validate_node_tree(ValidationContext& context, ComponentScope& scope, const Node& root);
void validate_component(ValidationContext& context, const Component& component);

// Module-level record type table checks: ids, names, sorted order, spans, members and cycles
// (sections 2.2 and 11). Must run before any component, so a value and a member type resolve.
void check_type_table(ValidationContext& context);
// Resolves the identifier a structurally valid type names against the type table, reporting
// `TESSERA_IR_UNKNOWN_TYPE` for an undeclared one and (with `mark_used`) marking a declared record
// as named by a component (section 2.2).
void validate_type_names(ValidationContext& context, std::string_view type, const SourceSpan& where,
                         bool mark_used = true);
// A `record` expression (section 4.3): its type, its field set and every field value. `expected` is
// the declared type of the position, or null when the position has none.
void validate_record_expr(ValidationContext& context, const Expr& expr, const std::string* expected,
                          const SourceSpan& where);
// Section 4.3: an initial-value position (a `useState` initial or a prop default) of
// `declared_type`. A record-typed position requires a record value of exactly that record; any
// other position runs the ordinary expression rules, which reject a record value there.
void validate_record_position(ValidationContext& context, const ComponentScope& scope,
                              const Expr* expr, const std::string& declared_type,
                              const SourceSpan& where);
// Section 4.3: a dotted `ref` into a declared record must name a member the record has.
void validate_reference_member(ValidationContext& context, const ComponentScope& scope,
                               std::string_view id, const SourceSpan& where);
// Section 9: closes `used_types` over the member types of the records it names, so a record used
// only as a member of another used record counts as used and a record reachable only from an unused
// record stays unused.
void close_used_types(ValidationContext& context);

} // namespace tessera::compiler::ir
