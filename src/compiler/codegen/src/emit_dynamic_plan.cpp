// SPDX-License-Identifier: MIT

// Collection and validation of the dynamic child forms: conditional branches and keyed list
// templates (docs/design/compiler.md section 11). The walk uses the same pre-order ordinal
// numbering as emit_node, counting template subtrees so ids, dynamic-text order and item ids agree.

#include "emit_dynamic.hpp"
#include "emit_dynamic_plan_util.hpp"
#include "emit_virtual.hpp"
#include "interpolation.hpp"
#include "value_ops.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::internal {
namespace {

// Small helpers live in emit_dynamic_plan_util.cpp so this file stays the plan walk itself.
using plan_detail::declared_field_type;
using plan_detail::fail;
using plan_detail::list_element_type;
using plan_detail::loop_source_type;
using plan_detail::mentions_loop_local;
using plan_detail::reserved_loop_local;
using plan_detail::starts_with;

struct PlanWalk {
    const ir::Component& component;
    // The module's record type table, so a text/key/condition position can resolve a dotted
    // member read (#827).
    std::span<const ir::RecordType> records;
    DynamicPlan& plan;
    std::string& error;
    unsigned ordinal = 0;
    bool in_template = false;
    unsigned template_ordinal = 0; // the enclosing template's loop-node ordinal while in a template
    std::size_t current_list = 0;  // index into plan.lists while walking a template subtree

    bool walk(const ir::Node& node, unsigned parent_ordinal, bool is_root,
              bool is_virtual_template_root = false);
    bool validate_template_node(const ir::Node& node, bool is_template_root);
    bool validate_template_loop(const ir::Node& node, const ir::Loop& loop,
                                std::string& element_type, std::string& element_cpp);
    bool collect_template(const ir::Node& node, unsigned node_ordinal, unsigned parent_ordinal);
    bool collect_virtual_collection(const ir::Node& node, unsigned node_ordinal);
    bool collect_text_site(const ir::Node& node, unsigned node_ordinal);
    [[nodiscard]] LocalBindings template_locals() const;
};

LocalBindings PlanWalk::template_locals() const {
    const ListTemplate& list = plan.lists[current_list];
    LocalBindings locals;
    locals.push_back(LocalBinding{list.item, list.item, list.element_type});
    if (!list.index.empty()) {
        locals.push_back(LocalBinding{list.index, "static_cast<int>(tessera_index)", "int"});
    }
    return locals;
}

bool PlanWalk::validate_template_node(const ir::Node& node, bool is_template_root) {
    if (node.kind == ir::NodeKind::Slot) {
        return fail(error, "`Slot` inside a list template (" + node.id +
                               ") is deferred; a per-item slot position has no caller content");
    }
    if (node.kind == ir::NodeKind::TextInput || node.kind == ir::NodeKind::TextArea) {
        return fail(error, "`" + std::string(ir::node_kind_name(node.kind)) +
                               "` inside a list template (" + node.id +
                               ") is deferred: a per-item edit controller needs per-item runtime "
                               "storage");
    }
    if (node.kind == ir::NodeKind::Component && current_list < plan.lists.size() &&
        plan.lists[current_list].is_virtual) {
        return fail(error,
                    "a component instance (`" + node.id +
                        "`) inside a virtual item template is deferred; use an eager list or "
                        "wrap the list in an element");
    }
    if (node.condition && current_list < plan.lists.size() && plan.lists[current_list].is_virtual) {
        // A virtual collection recycles item records by absolute index, so per-item presence inside
        // its template needs the presence state machine (element-transitions-v1, post-V1).
        return fail(error, "a conditional node (`" + node.id +
                               "`) inside a virtual item template is deferred; use an eager list");
    }
    if (node.loop && !is_template_root) {
        return fail(error, "a nested list template (`" + node.id +
                               "`) is deferred; v1 generates one list level per template");
    }
    // A conditional inside a template is per-item presence: the item record owns the branch slot
    // (docs/design/compiler-node-state.md section 4). Its expression is validated by walk().
    for (const ir::Ref& ref : component.refs) {
        if (ref.target == node.id) {
            return fail(error, "`useRef` `" + ref.name + "` targets `" + node.id +
                                   "`, which is inside a list template; one ref cannot name every "
                                   "generated item (deferred)");
        }
    }
    // Per-item metadata/style storage is deferred: a static value is painted by the item builder, a
    // dynamic one would need a per-item string table.
    for (const auto& [name, value] : node.props) {
        const bool metadata =
            name == "id" || name == "className" || name == "role" || starts_with(name, "aria-");
        if (metadata && value && !static_text_value(value.get())) {
            return fail(error, "dynamic `" + name + "` on `" + node.id +
                                   "` inside a list template is deferred (per-item metadata "
                                   "storage)");
        }
    }
    for (const auto& [name, value] : node.style) {
        if (value && !static_text_value(value.get())) {
            return fail(error, "dynamic inline style `" + name + "` on `" + node.id +
                                   "` inside a list template is deferred (per-item style storage)");
        }
    }
    return true;
}

bool PlanWalk::collect_text_site(const ir::Node& node, unsigned node_ordinal) {
    if (node.text == nullptr || static_text_value(node.text.get())) {
        return true; // no interpolation: the item builder paints the static payload once
    }
    ListTemplate& list = plan.lists[current_list];
    const LocalBindings locals = template_locals();
    if (!printable_text_expression(*node.text, component, records, locals)) {
        return fail(error, "the text of `" + node.id +
                               "` inside the list template has no printable C++ type");
    }
    list.text_sites.push_back(
        ListTextSite{node_ordinal, static_cast<unsigned>(list.text_sites.size()), node.text.get()});
    return true;
}

// Shared validation of an eager list template and a virtual item template: the loop locals, the
// source list and its element type, and the key expression. Fills `element_type`/`element_cpp`.

bool PlanWalk::validate_template_loop(const ir::Node& node, const ir::Loop& loop,
                                      std::string& element_type, std::string& element_cpp) {
    if (loop.item.empty()) {
        return fail(error, "a list template needs a loop item name");
    }
    if (reserved_loop_local(loop.item) ||
        (!loop.index.empty() && reserved_loop_local(loop.index))) {
        return fail(error, "the list callback parameters `" + loop.item + "`/`" + loop.index +
                               "` must not shadow generated code names (`node_*`, `tessera_*`, "
                               "`format_value`, ...); rename them");
    }
    if (loop.index == loop.item && !loop.index.empty()) {
        return fail(error, "a list callback index must differ from its item name");
    }
    // Section 5: a source is a list ref or a `filter`/`slice` of one; a derived list keeps its
    // innermost list's element type, so the declared `list<T>` comes from that ref (issue #830).
    const ir::Expr* source_ref =
        loop.source == nullptr ? nullptr : ir::list_source_ref(*loop.source);
    if (source_ref == nullptr) {
        return fail(error, "the loop source of `" + node.id +
                               "` must be a list reference or a `filter`/`slice` of one");
    }
    if (node.key == nullptr) {
        return fail(error, "the list template `" + node.id + "` needs a `key` expression");
    }
    // The declared `list<T>` of the innermost list. `loop_source_type` and `declared_field_type`
    // differ only for a derived source (issue #830): the former recurses to the ref that declares
    // the element type, the latter cannot resolve a `listFilter`'s synthesized id. Both agree for
    // a plain ref, so the recursive form covers every source the validator accepts.
    const std::optional<std::string> source_type = loop_source_type(component, *loop.source);
    if (!source_type) {
        return fail(error, "the list source `" + source_ref->id +
                               "` has no declared type; v1 lists are backed by a state or prop "
                               "`list<T>`");
    }
    const std::optional<std::string> element = list_element_type(*source_type);
    if (!element) {
        return fail(error, "the list source `" + source_ref->id + "` has type `" + *source_type +
                               "`, not `list<T>`");
    }
    // A printable primitive element (`bool`, `int`, `float`, `length`, `string`, `color`), or a
    // declared record whose fields the template reads (issue #834). Anything else has no C++ form.
    if (!text_value_type(*element)) {
        const bool is_record_element = [&] {
            for (const ir::RecordType& record : records) {
                if (record.name == *element) {
                    return true;
                }
            }
            return false;
        }();
        if (!is_record_element) {
            // The innermost list names the element type: a derived source's own id is not a
            // declaration, so `source_ref` is what a diagnostic can point at (issue #830).
            return fail(error, "the element type `" + *element + "` of `" + source_ref->id +
                                   "` has no printable C++ form in this slice; only bool/int/float/"
                                   "string/length/color element lists and declared records are "
                                   "generated");
        }
    }
    const std::optional<std::string> cpp = cpp_type(*element, records);
    if (!cpp) {
        return fail(error, "the element type `" + *element + "` has no C++ form in this slice");
    }
    element_type = *element;
    element_cpp = *cpp;
    LocalBindings locals;
    locals.push_back(LocalBinding{loop.item, loop.item, element_type});
    if (!loop.index.empty()) {
        locals.push_back(LocalBinding{loop.index, "static_cast<int>(tessera_index)", "int"});
    }
    const ExprEmission key = emit_expr(*node.key, component, locals);
    if (!key.error.empty()) {
        return fail(error, "the key of `" + node.id + "`: " + key.error);
    }
    if (!printable_text_expression(*node.key, component, records, locals)) {
        return fail(error, "the key of `" + node.id + "` has no printable C++ type");
    }
    if (!mentions_loop_local(*node.key, loop.item, loop.index)) {
        return fail(error, "the key of `" + node.id +
                               "` must read the loop item or index; a constant key gives every "
                               "item the same identity");
    }
    return true;
}

bool PlanWalk::collect_template(const ir::Node& node, unsigned node_ordinal,
                                unsigned parent_ordinal) {
    const ir::Loop& loop = *node.loop;
    ListTemplate list;
    list.ordinal = node_ordinal;
    list.parent_ordinal = parent_ordinal;
    list.node = &node;
    list.item = loop.item;
    list.index = loop.index;
    if (!validate_template_loop(node, loop, list.element_type, list.element_cpp)) {
        return false;
    }
    plan.lists.push_back(std::move(list));
    plan.has_lists = true;
    current_list = plan.lists.size() - 1;
    return true;
}

// ir-v1 section 3.5: a VirtualList/VirtualGrid owns exactly one static item template; it is not a
// child, so the plan registers it as a list template whose table is a bounded realized window.
bool PlanWalk::collect_virtual_collection(const ir::Node& node, unsigned node_ordinal) {
    if (!node.item_template) {
        return fail(error, "`" + std::string(ir::node_kind_name(node.kind)) + "` (`" + node.id +
                               "`) needs an itemTemplate");
    }
    const ir::Node& item = *node.item_template;
    if (!item.loop) {
        return fail(error, "the item template of `" + node.id +
                               "` carries the loop that names the item and the data source");
    }
    // Section 3.5: a virtual collection's item template still requires a list reference. A derived
    // source is accepted for an eager `.map` loop but is a follow-up for a realized window, so it
    // is rejected here rather than silently materializing the whole list.
    if (item.loop->source == nullptr || item.loop->source->kind != ir::ExprKind::Ref) {
        return fail(error, "the item template of `" + node.id +
                               "` must repeat over a list reference; a `filter`/`slice` source is "
                               "not supported by a virtual collection yet");
    }
    VirtualGeometrySpec geometry;
    if (!virtual_geometry_spec(component, node, geometry, error)) {
        return false;
    }
    ListTemplate list;
    list.ordinal = node_ordinal + 1; // the template is walked right after its collection
    list.parent_ordinal = node_ordinal;
    list.is_virtual = true;
    list.container_ordinal = node_ordinal;
    list.container = &node;
    list.geometry = std::move(geometry);
    list.node = &item;
    list.item = item.loop->item;
    list.index = item.loop->index;
    if (!validate_template_loop(item, *item.loop, list.element_type, list.element_cpp)) {
        return false;
    }
    plan.lists.push_back(std::move(list));
    plan.has_lists = true;
    plan.has_virtual = true;
    current_list = plan.lists.size() - 1;
    return true;
}

bool PlanWalk::walk(const ir::Node& node, unsigned parent_ordinal, bool is_root,
                    bool is_virtual_template_root) {
    const unsigned self = ++ordinal;
    plan.max_ordinal = self;
    switch (node.kind) {
    case ir::NodeKind::CustomElement:
        return fail(error, "`" + std::string(ir::node_kind_name(node.kind)) +
                               "` nodes are deferred (custom elements are a later codegen slice)");
    case ir::NodeKind::VirtualList:
    case ir::NodeKind::VirtualGrid:
        if (node.condition) {
            return fail(error, "a virtual collection (`" + node.id +
                                   "`) cannot be conditional; wrap it in a container");
        }
        if (node.loop) {
            return fail(error, "a virtual collection (`" + node.id +
                                   "`) is not itself looped; its item template carries the loop");
        }
        if (!collect_virtual_collection(node, self)) {
            return false;
        }
        break;
    case ir::NodeKind::Component:
        // Composition itself is emitted elsewhere. A conditional instance rides the branch-slot
        // path like any other conditional child (docs/design/compiler-node-state.md section 2); an
        // instance inside a template is owned by the item record (section 3). A `Component` node
        // that *carries* the loop is the template root, not a nested list.
        if (node.loop && in_template) {
            return fail(error, "a nested list template (`" + node.id +
                                   "`) is deferred; v1 generates one list level per template");
        }
        break;
    case ir::NodeKind::Slot:
        if (is_root) {
            return fail(error, "a Slot cannot be the component root");
        }
        if (node.condition || node.loop) {
            return fail(error, "a Slot (`" + node.id + "`) cannot be conditional or looped");
        }
        break;
    default:
        break;
    }
    const bool collection =
        node.kind == ir::NodeKind::VirtualList || node.kind == ir::NodeKind::VirtualGrid;
    if (node.item_template != nullptr && !collection) {
        return fail(error, "only VirtualList and VirtualGrid have an itemTemplate");
    }
    if (is_root && node.condition) {
        return fail(error, "the component root cannot be a conditional branch; wrap the condition "
                           "in a container element");
    }
    if (is_root && node.loop) {
        return fail(error, "the component root cannot be a loop template; wrap the list in a "
                           "container element");
    }
    if ((in_template || node.loop) && !validate_template_node(node, node.loop.has_value())) {
        return false;
    }
    if (node.loop && !is_virtual_template_root && !collect_template(node, self, parent_ordinal)) {
        return false;
    }
    // ADR-067 item 2: a Suspense boundary is a static node, so it is not a dynamic child of its own
    // parent; its *own* child set is the one that swaps. The fallback subtree is walked right after
    // the content subtree, which is the order emit_node emits them in, so every later ordinal stays
    // aligned with the other collectors.
    std::size_t suspense_index = 0;
    const bool suspense = node.kind == ir::NodeKind::Suspense;
    if (suspense) {
        SuspenseSite site;
        site.ordinal = self;
        site.read_count = 1; // the spike's one suspending read of the fake resource
        suspense_index = plan.suspenses.size();
        plan.suspenses.push_back(site);
    }
    if (node.condition) {
        // A condition inside a template reads the loop item/index (docs/design/compiler-node-state
        // .md section 4), so the check sees the enclosing template's locals.
        LocalBindings locals;
        if (in_template && current_list < plan.lists.size()) {
            const ListTemplate& owner = plan.lists[current_list];
            locals.push_back(LocalBinding{owner.item, owner.item, owner.element_type});
            if (!owner.index.empty()) {
                locals.push_back(
                    LocalBinding{owner.index, "static_cast<int>(tessera_index)", "int"});
            }
        }
        const ExprEmission condition = emit_expr(*node.condition, component, locals);
        if (!condition.error.empty()) {
            return fail(error, "the condition of `" + node.id + "`: " + condition.error);
        }
        if (!printable_text_expression(*node.condition, component, records, locals)) {
            return fail(error, "the condition of `" + node.id + "` has no printable C++ type");
        }
        plan.has_conditionals = true;
    }
    // Record the dynamic children of this node in IR order before recursing (their ordinals are
    // filled in as the walk reaches them). A node that carries the loop is the template root: its
    // own child set is per item, so the group belongs to the template it starts.
    DynamicGroup group;
    group.parent_ordinal = self;
    group.template_ordinal = node.loop ? self : (in_template ? template_ordinal : 0);
    bool dynamic_children = false;
    for (const ir::Node& child : node.children) {
        DynamicChild entry;
        entry.kind = child.condition ? DynamicChildKind::Conditional
                     : child.loop    ? DynamicChildKind::List
                                     : DynamicChildKind::Static;
        if (entry.kind != DynamicChildKind::Static) {
            entry.node = &child;
            dynamic_children = true;
        }
        group.children.push_back(entry);
    }
    std::size_t group_index = plan.groups.size();
    if (dynamic_children) {
        plan.groups.push_back(std::move(group));
    }
    if (node.loop || in_template) {
        if (node.text != nullptr && !collect_text_site(node, self)) {
            return false;
        }
    }
    // Recurse in IR order. A template root switches its subtree into template mode; the loop index
    // binding and the item budget are restored afterwards.
    const bool template_before = in_template;
    const unsigned template_ordinal_before = template_ordinal;
    const std::size_t list_before = current_list;
    if (node.loop) {
        in_template = true;
        template_ordinal = self;
    }
    // An instance inside a template (or the template root instance itself) is owned per item.
    if (node.kind == ir::NodeKind::Component && in_template && !plan.lists.empty()) {
        plan.lists[current_list].instance_ordinals.push_back(self);
    }
    std::size_t group_child = 0;
    for (const ir::Node& child : node.children) {
        const unsigned before = ordinal;
        if (!walk(child, self, false)) {
            return false;
        }
        if (dynamic_children &&
            plan.groups[group_index].children[group_child].kind != DynamicChildKind::Static) {
            plan.groups[group_index].children[group_child].ordinal = before + 1;
            plan.groups[group_index].children[group_child].node = &child;
        }
        ++group_child;
    }
    if (collection && node.item_template != nullptr) {
        // ir-v1 section 3.5: the item template is walked right after its collection node, so its
        // pre-order ordinals and its text sites line up with the generated item builder.
        in_template = true;
        template_ordinal = plan.lists[current_list].ordinal;
        if (!walk(*node.item_template, self, false, true)) {
            return false;
        }
        plan.lists[current_list].subtree_end = ordinal + 1;
        in_template = template_before;
        template_ordinal = template_ordinal_before;
        current_list = list_before;
    }
    if (suspense && node.fallback != nullptr) {
        // ADR-067 item 2: the fallback subtree is walked right after the content subtree, which is
        // the order emit_node emits them in, so every later ordinal stays aligned. The site is
        // addressed by the index captured at push time, not by reference: the recursive walk may
        // grow `plan.suspenses` (a nested boundary) and reallocate the vector.
        plan.suspenses[suspense_index].fallback_ordinal = ordinal + 1;
        if (!walk(*node.fallback, self, false)) {
            return false;
        }
        plan.suspenses[suspense_index].fallback_end = ordinal + 1;
    }
    if (node.loop) {
        plan.lists[current_list].subtree_end = ordinal + 1;
        in_template = template_before;
        template_ordinal = template_ordinal_before;
        current_list = list_before;
    }
    return true;
}

} // namespace

bool collect_dynamic_plan(const ir::Component& component, std::span<const ir::RecordType> records,
                          DynamicPlan& plan, std::string& error) {
    PlanWalk walk{component, records, plan, error};
    return walk.walk(component.root, 0, true);
}

} // namespace tessera::compiler::codegen::internal
