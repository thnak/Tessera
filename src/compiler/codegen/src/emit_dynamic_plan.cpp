// SPDX-License-Identifier: MIT

// Collection and validation of the dynamic child forms: conditional branches and keyed list
// templates (docs/design/compiler.md section 11). The walk uses the same pre-order ordinal
// numbering as emit_node, counting template subtrees so ids, dynamic-text order and item ids agree.

#include "emit_dynamic.hpp"
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

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

// Generated item builders bind the loop item/index by name, so a name that collides with a
// generated local, the generated id prefix or a helper the item body calls would not compile (or
// would silently resolve to the wrong object). Rejecting them keeps the generated code readable.
bool reserved_loop_local(std::string_view name) {
    static constexpr std::array<std::string_view, 11> kReserved = {
        "format_value",  "apply_state", "attach_child_at", "detach_child", "kEffectHashSeed",
        "kItemIdStride", "build_item",  "apply_item",      "apply_list",   "apply_children"};
    if (starts_with(name, "tessera_") || starts_with(name, "node_")) {
        return true;
    }
    return std::find(kReserved.begin(), kReserved.end(), name) != kReserved.end();
}

// The declared IR type of a component field reference (`state::rows` -> `list<string>`).
std::optional<std::string> declared_field_type(const ir::Component& component,
                                               std::string_view ref_id) {
    for (const ir::State& state : component.state) {
        if (state.id == ref_id) {
            return state.type;
        }
    }
    if (starts_with(ref_id, "prop::")) {
        const std::string_view name = ref_id.substr(6);
        for (const ir::Prop& prop : component.props) {
            if (prop.name == name) {
                return prop.type;
            }
        }
    }
    return std::nullopt;
}

// `list<T>` -> `T`.
std::optional<std::string> list_element_type(std::string_view type) {
    if (!starts_with(type, "list<") || type.back() != '>') {
        return std::nullopt;
    }
    return std::string(type.substr(5, type.size() - 6));
}

// True when `expr` reads the loop item or index (directly or as `item.field`). A key that reads
// only component state would give every item the same key, so it is rejected instead of silently
// degrading to a positional match.
bool mentions_loop_local(const ir::Expr& expr, std::string_view item, std::string_view index) {
    switch (expr.kind) {
    case ir::ExprKind::Ref: {
        const std::string_view base = std::string_view(expr.id).substr(0, expr.id.find('.'));
        return base == item || (!index.empty() && base == index);
    }
    case ir::ExprKind::Literal:
        return false;
    case ir::ExprKind::Template:
        for (const ir::TemplatePart& part : expr.parts) {
            if (part.expr && mentions_loop_local(*part.expr, item, index)) {
                return true;
            }
        }
        return false;
    default:
        for (const ir::ExprPtr& operand : expr.operands) {
            if (operand && mentions_loop_local(*operand, item, index)) {
                return true;
            }
        }
        return false;
    }
}

struct PlanWalk {
    const ir::Component& component;
    DynamicPlan& plan;
    std::string& error;
    unsigned ordinal = 0;
    bool in_template = false;
    std::size_t current_list = 0; // index into plan.lists while walking a template subtree

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
    if (node.kind == ir::NodeKind::Component || node.kind == ir::NodeKind::Slot) {
        return fail(error, "`" + std::string(ir::node_kind_name(node.kind)) +
                               "` inside a list template (" + node.id +
                               ") is deferred: a per-item component instance needs per-item "
                               "state");
    }
    if (node.kind == ir::NodeKind::TextInput || node.kind == ir::NodeKind::TextArea) {
        return fail(error, "`" + std::string(ir::node_kind_name(node.kind)) +
                               "` inside a list template (" + node.id +
                               ") is deferred: a per-item edit controller needs per-item runtime "
                               "storage");
    }
    if (node.loop && !is_template_root) {
        return fail(error, "a nested list template (`" + node.id +
                               "`) is deferred; v1 generates one list level per template");
    }
    if (node.condition) {
        return fail(error, "a conditional node (`" + node.id +
                               "`) inside a list template is deferred: a per-item child set needs "
                               "per-item presence state");
    }
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
    if (!printable_text_expression(*node.text, component, locals)) {
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
    if (loop.source == nullptr || loop.source->kind != ir::ExprKind::Ref) {
        return fail(error, "the loop source of `" + node.id + "` must be a list reference");
    }
    if (node.key == nullptr) {
        return fail(error, "the list template `" + node.id + "` needs a `key` expression");
    }
    const std::optional<std::string> source_type = declared_field_type(component, loop.source->id);
    if (!source_type) {
        return fail(error, "the list source `" + loop.source->id +
                               "` has no declared type; v1 lists are backed by a state or prop "
                               "`list<T>`");
    }
    const std::optional<std::string> element = list_element_type(*source_type);
    if (!element) {
        return fail(error, "the list source `" + loop.source->id + "` has type `" + *source_type +
                               "`, not `list<T>`");
    }
    if (!text_value_type(*element)) {
        return fail(error, "the element type `" + *element + "` of `" + loop.source->id +
                               "` has no printable C++ form in this slice; only bool/int/float/"
                               "string/length/color element lists are generated");
    }
    const std::optional<std::string> cpp = cpp_type(*element);
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
    if (!printable_text_expression(*node.key, component, locals)) {
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
    if (!validate_virtual_geometry(node, error)) {
        return false;
    }
    ListTemplate list;
    list.ordinal = node_ordinal + 1; // the template is walked right after its collection
    list.parent_ordinal = node_ordinal;
    list.is_virtual = true;
    list.container_ordinal = node_ordinal;
    list.container = &node;
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
        // Composition itself is emitted elsewhere; the instance forms it cannot express yet are
        // rejected there, and a conditional or looped instance is rejected here.
        if (node.condition || node.loop) {
            return fail(error, "a conditional or looped component instance (`" + node.id +
                                   "`) is deferred; wrap the element in a container");
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
    if (node.condition) {
        const ExprEmission condition = emit_expr(*node.condition, component, {});
        if (!condition.error.empty()) {
            return fail(error, "the condition of `" + node.id + "`: " + condition.error);
        }
        if (!printable_text_expression(*node.condition, component, {})) {
            return fail(error, "the condition of `" + node.id + "` has no printable C++ type");
        }
        plan.has_conditionals = true;
    }
    // Record the dynamic children of this node in IR order before recursing (their ordinals are
    // filled in as the walk reaches them).
    DynamicGroup group;
    group.parent_ordinal = self;
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
    const std::size_t list_before = current_list;
    if (node.loop) {
        in_template = true;
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
        if (!walk(*node.item_template, self, false, true)) {
            return false;
        }
        plan.lists[current_list].subtree_end = ordinal + 1;
        in_template = template_before;
        current_list = list_before;
    }
    if (node.loop) {
        plan.lists[current_list].subtree_end = ordinal + 1;
        in_template = template_before;
        current_list = list_before;
    }
    return true;
}

} // namespace

const DynamicGroup* DynamicPlan::group(unsigned parent_ordinal) const {
    for (const DynamicGroup& group : groups) {
        if (group.parent_ordinal == parent_ordinal) {
            return &group;
        }
    }
    return nullptr;
}

const ListTemplate* DynamicPlan::list(unsigned ordinal) const {
    for (const ListTemplate& list : lists) {
        if (list.ordinal == ordinal) {
            return &list;
        }
    }
    return nullptr;
}

const ListTemplate* DynamicPlan::virtual_collection(unsigned container_ordinal) const {
    for (const ListTemplate& list : lists) {
        if (list.is_virtual && list.container_ordinal == container_ordinal) {
            return &list;
        }
    }
    return nullptr;
}

DynamicChildKind DynamicPlan::child_kind(unsigned parent_ordinal, unsigned child_ordinal) const {
    const DynamicGroup* owning = group(parent_ordinal);
    if (owning == nullptr) {
        return DynamicChildKind::Static;
    }
    for (const DynamicChild& child : owning->children) {
        if (child.kind != DynamicChildKind::Static && child.ordinal == child_ordinal) {
            return child.kind;
        }
    }
    return DynamicChildKind::Static;
}

std::string branch_member(unsigned ordinal, std::string_view suffix) {
    return "branch_" + std::to_string(ordinal) + "_" + std::string(suffix);
}

bool collect_dynamic_plan(const ir::Component& component, DynamicPlan& plan, std::string& error) {
    PlanWalk walk{component, plan, error};
    return walk.walk(component.root, 0, true);
}

} // namespace tessera::compiler::codegen::internal
