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
    // Non-zero when the group's parent is inside a list template: the walk then runs per item, over
    // the item record's own branch slots and group parent, not over the component's members
    // (docs/design/compiler-node-state.md section 4).
    unsigned template_ordinal = 0;

    [[nodiscard]] bool in_template() const {
        return template_ordinal != 0;
    }
};

// One interpolated text node inside a list template: the per-item content pass rebuilds it from the
// loop item/index values (the template expressions reference loop locals).
struct ListTextSite {
    unsigned ordinal = 0; // pre-order ordinal of the Text node
    unsigned site = 0;    // index in the template's text list (Item_<ordinal>::texts)
    const ir::Expr* text = nullptr;
};

// One collection-level event target of a virtual collection (`onScroll`, `onEndReached`). Both
// targets are generated member functions: an inline handler's `handler_<n>` or a `bind::` region.
struct VirtualEventTarget {
    bool present = false;
    std::string member;   // generated member function name
    bool payload = false; // the target takes the scene `ScrollEventInfo` payload
};

// The collection-level events a virtual collection declares (virtualization-v1.md section 10).
// They are driven by the generated `scroll_virtual_<n>` entry point, not by a scene `EventType`:
// the scene has no scroll event type yet, and the collection owns the scroll offset.
struct VirtualEventWiring {
    VirtualEventTarget on_scroll;
    VirtualEventTarget on_end_reached;
};

// One numeric geometry input of a virtual collection (virtualization-v1.md section 10). `cpp` is
// the emitted C++ expression: a literal such as `32.0`, or `static_cast<double>(<member>)` for a
// value `update_virtual_geometry_<n>()` re-evaluates from a state/prop cell.
struct VirtualGeometryValue {
    bool present = false; // the collection declares this input
    bool literal = false; // a number literal (compile-time; never re-evaluated)
    double literal_value = 0.0;
    std::string cpp;
};

// One structural geometry input (`orientation`, `columns`, `overscan`, `columnWidth`). A literal is
// emitted as a `static constexpr` member, byte-identical to the literal-only emission; a component
// prop reference makes it a mutable member that `scroll_virtual_<n>()` refreshes from the generated
// props field at sync time (virtualization-v1.md section 10).
struct VirtualOption {
    bool prop_driven = false; // the value comes from a `props.<name>` reference
    std::string cpp;          // generated props member expression when prop-driven
};

// The geometry of one virtual collection, resolved once by the plan walk: the values the generated
// `ItemGeometry` seeds from plus the rule that says whether a change invalidates measurements.
struct VirtualGeometrySpec {
    bool grid = false;
    bool horizontal = false;           // literal main axis: x instead of y
    VirtualGeometryValue fixed_extent; // list `itemSize` / grid `rowHeight` (absent means variable)
    VirtualGeometryValue estimate;     // effective estimate for unmeasured items
    VirtualGeometryValue gap;          // inter-item main-axis gap
    std::int64_t columns = 0;          // literal grid columns; 0 means column-width mode
    double overscan = 200.0;           // literal main-axis overscan distance
    double column_width = 0.0;         // literal grid column extent (column-width mode)
    VirtualOption orientation_source;  // prop-driven orientation (empty for a literal)
    VirtualOption columns_source;      // prop-driven grid column count
    VirtualOption overscan_source;     // prop-driven overscan distance
    VirtualOption column_width_source; // prop-driven grid column extent
    // A declared fixed extent seeds the effective estimate only when no `estimateSize` overrides
    // it, so only then does changing it invalidate the recorded measurements.
    bool fixed_seeds_estimate = false;
};

struct ListTemplate {
    unsigned ordinal = 0;           // the template node's pre-order ordinal
    unsigned subtree_end = 0;       // one past the last ordinal of the template subtree
    unsigned parent_ordinal = 0;    // the owning group's parent ordinal
    bool is_virtual = false;        // a VirtualList/VirtualGrid item template (ir-v1 section 3.5)
    unsigned container_ordinal = 0; // the collection node's ordinal when `is_virtual`
    const ir::Node* node = nullptr;
    const ir::Node* container = nullptr; // the collection node when `is_virtual`
    std::string item;                    // loop item local name
    std::string index;                   // loop index local name (empty when the callback has none)
    std::string element_type; // IR element type of the source list (`string`, `int`, ...)
    std::string element_cpp;  // C++ element type (`std::string`, `int`, ...)
    std::vector<ListTextSite> text_sites;
    // Ordinals of the `Component` nodes in this template subtree: one owned instance per item
    // (docs/design/compiler-node-state.md section 3).
    std::vector<unsigned> instance_ordinals;
    VirtualEventWiring events;    // collection-level events (virtual collections only)
    VirtualGeometrySpec geometry; // resolved geometry inputs (virtual collections only)
};

struct DynamicPlan {
    std::vector<DynamicGroup> groups; // ascending parent ordinal (IR walk order)
    std::vector<ListTemplate> lists;  // ascending template ordinal
    unsigned max_ordinal = 0;         // last pre-order ordinal (item ids use max_ordinal + 1)
    bool has_conditionals = false;
    bool has_lists = false;
    bool has_virtual = false; // at least one VirtualList/VirtualGrid item template

    [[nodiscard]] const DynamicGroup* group(unsigned parent_ordinal) const;
    [[nodiscard]] const ListTemplate* list(unsigned ordinal) const;
    // The item template owned by a virtual collection node with `container_ordinal`.
    [[nodiscard]] const ListTemplate* virtual_collection(unsigned container_ordinal) const;
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
void emit_dynamic_item_structs(CppWriter& writer, const DynamicPlan& plan,
                               const std::vector<ComponentInstance>& instances);
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

// Emits `<Name>::apply_item_children_<p>(Item_<n>&, const T&, std::size_t)`: the same walk for a
// dynamic child set inside a list item, over the item record's own branch slots
// (docs/design/compiler-node-state.md section 4).
[[nodiscard]] bool emit_item_apply_children(CppWriter& writer, const ir::Component& component,
                                            const DynamicPlan& plan, const DynamicGroup& group,
                                            std::string& error);

// Emits build_item_<n>(), apply_item_<n>() and apply_list_<n>() for every list template
// (emit_list.cpp). A virtual item template emits apply_virtual_<n>() instead of apply_list_<n>().
[[nodiscard]] bool emit_list_sources(CppWriter& writer, const ir::Component& component,
                                     const ir::IrModule& module, const DynamicPlan& plan,
                                     const std::vector<EventAdapter>& adapters,
                                     const std::vector<DynamicText>& dynamic_text,
                                     const std::vector<DynamicMetadata>& dynamic_metadata,
                                     const std::vector<ControlledInput>& controlled_inputs,
                                     const std::vector<ComponentInstance>& instances,
                                     std::string& error);

// Emits build_item_<n>(), apply_item_<n>(), apply_list_<n>() and apply_children_<p>() for the plan.
[[nodiscard]] bool emit_dynamic_sources(CppWriter& writer, const ir::Component& component,
                                        const ir::IrModule& module, const DynamicPlan& plan,
                                        const std::vector<EventAdapter>& adapters,
                                        const std::vector<DynamicText>& dynamic_text,
                                        const std::vector<DynamicMetadata>& dynamic_metadata,
                                        const std::vector<ControlledInput>& controlled_inputs,
                                        const std::vector<ComponentInstance>& instances,
                                        std::string& error);

// Calls `method` on every live per-item component instance of the plan (for example `mount`,
// `update`, `unmount`, `attach_scheduler`). `argument` is appended inside the call parentheses
// (`""` for the parameterless lifecycle methods). `reverse` walks each item table backwards, which
// the teardown paths need (a later item unmounts first).
void emit_item_instance_calls(CppWriter& writer, const DynamicPlan& plan, std::string_view method,
                              std::string_view argument = {}, bool reverse = false);

// True when the plan owns at least one per-item component instance.
[[nodiscard]] bool plan_has_item_instances(const DynamicPlan& plan);

// The generated member name for one conditional branch slot (`branch_7_owned_`).
[[nodiscard]] std::string branch_member(unsigned ordinal, std::string_view suffix);

} // namespace tessera::compiler::codegen::internal
