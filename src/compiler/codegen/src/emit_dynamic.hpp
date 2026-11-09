// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// IR -> generated C++ for the two dynamic child forms (docs/design/compiler.md section 11):
//
// - a conditional child is a sibling node carrying `condition`; lowering emits one branch per
//   possible child (a ternary emits two complementary branches), so codegen builds every branch
//   subtree once in build() and apply_state() attaches/detaches the present one at its source
//   position;
// - a list template is one node carrying `loop` plus `key`; codegen generates a per-item builder,
//   a bounded item table keyed by the lowered key expression, and a per-item content pass.
namespace tessera::compiler::codegen::internal {

// How one child of a parent participates in the generated child-set update.
enum class DynamicChildKind : std::uint8_t { Static, Conditional, List };

struct DynamicChild {
    DynamicChildKind kind = DynamicChildKind::Static;
    unsigned ordinal = 0;           // the child's pre-order ordinal
    const ir::Node* node = nullptr; // the IR child (nullptr for Static entries)
};

// A parent node whose child set is dynamic. `children` lists every child in IR order so the
// generated update walk can compute deterministic sibling positions: a static child always advances
// the position, a conditional branch advances it only while present, and a list advances it by its
// generated item count.
struct DynamicGroup {
    unsigned parent_ordinal = 0;
    std::vector<DynamicChild> children;
};

// One interpolated text node inside a list template: the per-item content pass rebuilds it from the
// loop item/index values (the template expressions reference loop locals).
struct ListTextSite {
    unsigned ordinal = 0; // pre-order ordinal of the Text node
    unsigned site = 0;    // index in the template's text list (Item_<ordinal>::texts)
    const ir::Expr* text = nullptr;
};

struct ListTemplate {
    unsigned ordinal = 0;        // the template node's pre-order ordinal
    unsigned subtree_end = 0;    // one past the last ordinal of the template subtree
    unsigned parent_ordinal = 0; // the owning group's parent ordinal
    const ir::Node* node = nullptr;
    std::string item;         // loop item local name
    std::string index;        // loop index local name (empty when the callback has none)
    std::string element_type; // IR element type of the source list (`string`, `int`, ...)
    std::string element_cpp;  // C++ element type (`std::string`, `int`, ...)
    std::vector<ListTextSite> text_sites;
};

struct DynamicPlan {
    std::vector<DynamicGroup> groups; // ascending parent ordinal (IR walk order)
    std::vector<ListTemplate> lists;  // ascending template ordinal
    unsigned max_ordinal = 0;         // last pre-order ordinal (item ids use max_ordinal + 1)
    bool has_conditionals = false;
    bool has_lists = false;

    [[nodiscard]] const DynamicGroup* group(unsigned parent_ordinal) const;
    [[nodiscard]] const ListTemplate* list(unsigned ordinal) const;
    // The kind of `child_ordinal` among the children of `parent_ordinal`.
    [[nodiscard]] DynamicChildKind child_kind(unsigned parent_ordinal,
                                              unsigned child_ordinal) const;
};

// Walks the component tree once, validates the dynamic forms this slice supports, and fills `plan`.
// The ordinal numbering matches emit_node/the other collectors (pre-order over every node, template
// subtrees included). Unsupported forms are reported as TESSERA_CODEGEN_UNSUPPORTED.
[[nodiscard]] bool collect_dynamic_plan(const ir::Component& component, DynamicPlan& plan,
                                        std::string& error);

// Header emission: the bounded item cap/item structs (namespace scope, before the component
// struct), the generated members, and the method declarations.
void emit_dynamic_item_structs(CppWriter& writer, const DynamicPlan& plan);
void emit_dynamic_fields(CppWriter& writer, const DynamicPlan& plan);
void emit_dynamic_declarations(CppWriter& writer, const DynamicPlan& plan);

// Generated .cpp file-local helpers: the item-id stride constant and the attach/detach slot
// helpers.
void emit_dynamic_helpers(CppWriter& writer, const DynamicPlan& plan);

struct EventAdapter;

// Emits one `<Name>::apply_children_<p>()` (emit_conditional.cpp): the deterministic attach/detach
// walk over one parent's child slots.
[[nodiscard]] bool emit_apply_children(CppWriter& writer, const ir::Component& component,
                                       const DynamicPlan& plan, const DynamicGroup& group,
                                       std::string& error);

// Emits build_item_<n>(), apply_item_<n>() and apply_list_<n>() for every list template
// (emit_list.cpp).
[[nodiscard]] bool emit_list_sources(CppWriter& writer, const ir::Component& component,
                                     const DynamicPlan& plan,
                                     const std::vector<EventAdapter>& adapters,
                                     const std::vector<DynamicText>& dynamic_text,
                                     const std::vector<DynamicMetadata>& dynamic_metadata,
                                     const std::vector<ControlledInput>& controlled_inputs,
                                     std::string& error);

// Emits build_item_<n>(), apply_item_<n>(), apply_list_<n>() and apply_children_<p>() for the plan.
[[nodiscard]] bool emit_dynamic_sources(CppWriter& writer, const ir::Component& component,
                                        const DynamicPlan& plan,
                                        const std::vector<EventAdapter>& adapters,
                                        const std::vector<DynamicText>& dynamic_text,
                                        const std::vector<DynamicMetadata>& dynamic_metadata,
                                        const std::vector<ControlledInput>& controlled_inputs,
                                        std::string& error);

// The generated member name for one conditional branch slot (`branch_7_owned_`).
[[nodiscard]] std::string branch_member(unsigned ordinal, std::string_view suffix);

} // namespace tessera::compiler::codegen::internal
