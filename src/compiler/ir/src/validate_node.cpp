// SPDX-License-Identifier: MIT

// Node tree checks (docs/specs/ir-v1.md sections 3, 3.1, 3.2, 3.3, 3.4, 5 and 5.1).

#include "validate_internal.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace tessera::compiler::ir {
namespace {

constexpr int kMaxLoopDepth = 2; // section 5

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

// A loop item or index is an action-style local name (subset 10.5 callback parameter).
bool valid_local_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    const auto word_start = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
    };
    const auto word = [&](char c) { return word_start(c) || (c >= '0' && c <= '9'); };
    if (!word_start(name.front())) {
        return false;
    }
    for (const char c : name) {
        if (!word(c)) {
            return false;
        }
    }
    return true;
}

struct NodeWalker {
    ValidationContext& context;
    ComponentScope& scope;
    unsigned slot_count = 0; // section 3.4: at most one Slot per component

    void walk(const Node& node, const LocalNames& outer, int loop_depth, bool in_slot_content,
              bool is_root);
    void check_identity(const Node& node);
    void check_shape(const Node& node);
    void check_events(const Node& node);
    void check_condition(const Node& node);
    void check_loop(const Node& node, const LocalNames& outer, int loop_depth);
    void check_slot(const Node& node, int loop_depth, bool in_slot_content, bool is_root);
};

bool declares_children(const Component& component) {
    for (const auto& prop : component.props) {
        if (prop.name == "children" && prop.type == "children") {
            return true;
        }
    }
    return false;
}

void NodeWalker::check_identity(const Node& node) {
    if (node.id.empty()) {
        context.report("TESSERA_IR_MISSING_ID", "a node has no id", node.source);
    } else if (!scope.node_ids.insert(node.id).second) {
        context.report("TESSERA_IR_DUPLICATE_ID", "duplicate node id `" + node.id + "`",
                       node.source);
    }
    if (node.ui_id.empty()) {
        context.report("TESSERA_IR_MISSING_ID", "node `" + node.id + "` has no uiId", node.source);
    } else if (!context.ui_ids.insert(node.ui_id).second) {
        context.report("TESSERA_IR_DUPLICATE_ID", "uiId `" + node.ui_id + "` is used twice",
                       node.source);
    }
    if (!node.automation_id.empty() && node.automation_id != node.ui_id) {
        context.report("TESSERA_IR_AUTOMATION_ID",
                       "automationId `" + node.automation_id + "` must equal uiId", node.source);
    }
    validate_span(context, node.source, "node `" + node.id + "`");
}

void NodeWalker::check_shape(const Node& node) {
    const bool custom = node.kind == NodeKind::CustomElement;
    const bool component = node.kind == NodeKind::Component;
    if (custom && node.element.empty()) {
        context.report("TESSERA_IR_UNRESOLVED", "a CustomElement needs a literal `element`",
                       node.source);
    }
    if (!custom && !node.element.empty()) {
        context.report("TESSERA_IR_MALFORMED", "only a CustomElement has `element`", node.source);
    }
    if (component && context.component_ids.count(node.component) == 0) {
        context.report("TESSERA_IR_UNRESOLVED", "unresolved component `" + node.component + "`",
                       node.source);
    }
    if (component && !node.children.empty()) {
        // Section 3.4: only a component that declares the `children` slot prop accepts slot
        // content.
        const auto target = context.components.find(node.component);
        if (target != context.components.end() && !declares_children(*target->second)) {
            context.report("TESSERA_IR_SLOT",
                           "a Component node has slot children, but `" + node.component +
                               "` does not declare a `children` prop",
                           node.source);
        }
    }
    if (node.kind == NodeKind::Slot) {
        // Section 3.4: a Slot is a bare position marker; the caller's nodes fill it at run time.
        const bool has_payload = !node.props.empty() || !node.style.empty() ||
                                 !node.events.empty() || node.text || node.key || node.condition ||
                                 node.loop || node.fallback || node.item_template ||
                                 !node.children.empty() || !node.element.empty() ||
                                 !node.component.empty() || !node.automation_id.empty();
        if (has_payload) {
            context.report("TESSERA_IR_SLOT",
                           "a Slot is a leaf with no props, style, events, key, condition, loop or "
                           "children",
                           node.source);
        }
    }
    if (!component && !node.component.empty()) {
        context.report("TESSERA_IR_MALFORMED", "only a Component node has `component`",
                       node.source);
    }
    if (component && !node.style.empty()) {
        context.report("TESSERA_IR_MALFORMED",
                       "a Component node carries props, not style (section 3.4)", node.source);
    }
    if (node.text && node.kind != NodeKind::Text) {
        context.report("TESSERA_IR_MALFORMED", "only a Text node has `text`", node.source);
    }
    // A CustomElement, TextInput and TextArea are leaves; a Component node may carry the caller's
    // slot children (section 3.4), validated against the target props by the type pass.
    const bool leaf = custom || node.kind == NodeKind::TextInput || node.kind == NodeKind::TextArea;
    if (leaf && !node.children.empty()) {
        context.report("TESSERA_IR_LEAF",
                       std::string(node_kind_name(node.kind)) + " must not have children",
                       node.source);
    }
    if (node.kind == NodeKind::ErrorBoundary) {
        if (!node.fallback) {
            context.report("TESSERA_IR_ERROR_BOUNDARY", "an ErrorBoundary needs a fallback",
                           node.source);
        } else if (node.fallback->condition || node.fallback->loop) {
            context.report("TESSERA_IR_ERROR_BOUNDARY",
                           "an ErrorBoundary fallback is a static element, not conditional or "
                           "looped (section 5.1)",
                           node.fallback->source);
        }
        if (node.children.size() != 1) {
            context.report("TESSERA_IR_ERROR_BOUNDARY",
                           "an ErrorBoundary needs exactly one child subtree", node.source);
        }
    } else if (node.fallback) {
        context.report("TESSERA_IR_MALFORMED", "only an ErrorBoundary has a fallback", node.source);
    }
    const bool collection =
        node.kind == NodeKind::VirtualList || node.kind == NodeKind::VirtualGrid;
    if (collection) {
        // Section 3.5: one static template; the data source is the template's loop source.
        if (!node.item_template) {
            context.report("TESSERA_IR_VIRTUAL", "a virtual collection needs an itemTemplate",
                           node.source);
        } else if (!node.item_template->loop) {
            context.report("TESSERA_IR_VIRTUAL",
                           "an itemTemplate carries the loop that names the item and the data",
                           node.item_template->source);
        } else if (node.item_template->condition) {
            context.report("TESSERA_IR_VIRTUAL", "an itemTemplate is static, not conditional",
                           node.item_template->source);
        }
        if (!node.children.empty()) {
            context.report("TESSERA_IR_VIRTUAL",
                           "a virtual collection has an itemTemplate, not children", node.source);
        }
        if (node.loop) {
            context.report("TESSERA_IR_VIRTUAL", "a virtual collection is not itself looped",
                           node.source);
        }
    } else if (node.item_template) {
        context.report("TESSERA_IR_MALFORMED",
                       "only VirtualList and VirtualGrid have an itemTemplate", node.source);
    }
}

void NodeWalker::check_events(const Node& node) {
    for (const auto& [name, target] : node.events) {
        if (starts_with(target, "bind::")) {
            const auto found = context.bindings.find(target);
            if (found == context.bindings.end() ||
                found->second->component != scope.component->id) {
                context.report("TESSERA_IR_UNRESOLVED",
                               "event `" + name + "` names unknown binding `" + target + "`",
                               node.source);
            } else if (found->second->kind != BindingKind::Event) {
                context.report("TESSERA_IR_BINDING",
                               "event `" + name + "` names `" + target +
                                   "`, which is not an event binding (section 6)",
                               node.source);
            } else {
                context.used_bindings.insert(target);
            }
        } else if (starts_with(target, "handler::")) {
            if (scope.handlers.count(target) == 0) {
                context.report("TESSERA_IR_UNRESOLVED",
                               "event `" + name + "` names unknown handler `" + target + "`",
                               node.source);
            }
        } else {
            context.report("TESSERA_IR_MALFORMED",
                           "event `" + name + "` must name a bind:: or handler:: id", node.source);
        }
    }
}

void NodeWalker::check_loop(const Node& node, const LocalNames& outer, int loop_depth) {
    const Loop& loop = *node.loop;
    if (loop_depth + 1 > kMaxLoopDepth) {
        context.report("TESSERA_IR_LOOP", "dynamic loops nest at most 2 deep", node.source);
    }
    if (loop.item.empty()) {
        context.report("TESSERA_IR_MALFORMED", "a loop needs an item name", node.source);
    } else if (!valid_local_name(loop.item)) {
        context.report("TESSERA_IR_LOOP", "a loop item must be a local name", node.source);
    }
    if (!loop.index.empty() && (!valid_local_name(loop.index) || loop.index == loop.item)) {
        context.report("TESSERA_IR_LOOP",
                       "a loop index must be a local name different from the item", node.source);
    }
    if (!loop.source || loop.source->kind != ExprKind::Ref) {
        context.report("TESSERA_IR_UNSUPPORTED", "a loop source must be a list reference",
                       node.source);
    } else {
        validate_expr(context, scope, outer, loop.source.get(), node.source);
    }
    if (!node.key) {
        context.report("TESSERA_IR_LIST_KEY", "a node that carries a loop needs a key",
                       node.source);
    }
}

// Section 3: `condition` is a boolean-valued expression. Types are the type pass; the structural
// part is that a template (always a string) or a non-bool literal cannot be a condition.
void NodeWalker::check_condition(const Node& node) {
    if (!node.condition) {
        return;
    }
    if (node.condition->kind == ExprKind::Template) {
        context.report("TESSERA_IR_CONDITION",
                       "a condition is boolean-valued; a template is a string", node.source);
    } else if (node.condition->kind == ExprKind::Literal &&
               !std::holds_alternative<bool>(node.condition->literal)) {
        context.report("TESSERA_IR_CONDITION", "a condition literal must be a bool", node.source);
    }
}

// Section 3.4: one Slot, only in a component that declares the `children` prop, never as the root
// and never inside a loop template or another component's slot content.
void NodeWalker::check_slot(const Node& node, int loop_depth, bool in_slot_content, bool is_root) {
    if (!scope.declares_children) {
        context.report("TESSERA_IR_SLOT",
                       "a Slot needs the component's `children` prop (section 3.4)", node.source);
    }
    if (is_root) {
        context.report("TESSERA_IR_SLOT", "a Slot cannot be the component root", node.source);
    }
    if (++slot_count > 1) {
        context.report("TESSERA_IR_SLOT", "a component has at most one Slot", node.source);
    }
    if (loop_depth > 0) {
        context.report("TESSERA_IR_SLOT", "a Slot is not inside a loop template", node.source);
    }
    if (in_slot_content) {
        context.report("TESSERA_IR_SLOT", "a Slot is not inside another component's slot content",
                       node.source);
    }
}

void NodeWalker::walk(const Node& node, const LocalNames& outer, int loop_depth,
                      bool in_slot_content, bool is_root) {
    check_identity(node);
    check_shape(node);
    check_events(node);
    if (node.kind == NodeKind::Slot) {
        check_slot(node, loop_depth, in_slot_content, is_root);
    }

    LocalNames locals = outer;
    if (node.loop) {
        check_loop(node, outer, loop_depth);
        locals.push_back(node.loop->item);
        if (!node.loop->index.empty()) {
            locals.push_back(node.loop->index);
        }
        ++loop_depth;
    } else if (node.key && node.key->kind != ExprKind::Literal) {
        context.report("TESSERA_IR_MALFORMED", "a key outside a loop must be a literal",
                       node.source);
    }
    if (node.key) {
        validate_expr(context, scope, locals, node.key.get(), node.source);
    }
    for (const auto& [name, value] : node.props) {
        require_expr(context, scope, locals, value.get(), node.source, "prop `" + name + "`");
    }
    for (const auto& [name, value] : node.style) {
        require_expr(context, scope, locals, value.get(), node.source, "style `" + name + "`");
    }
    validate_expr(context, scope, locals, node.text.get(), node.source);
    check_condition(node);
    validate_expr(context, scope, locals, node.condition.get(), node.source);
    // The children of a Component node are the caller's slot content (section 3.4).
    const bool children_in_slot = in_slot_content || node.kind == NodeKind::Component;
    for (const auto& child : node.children) {
        walk(child, locals, loop_depth, children_in_slot, false);
    }
    if (node.fallback) {
        walk(*node.fallback, locals, loop_depth, in_slot_content, false);
    }
    if (node.item_template) {
        walk(*node.item_template, locals, loop_depth, in_slot_content, false);
    }
}

} // namespace

void validate_node_tree(ValidationContext& context, ComponentScope& scope, const Node& root) {
    NodeWalker walker{context, scope};
    walker.walk(root, {}, 0, false, true);
}

} // namespace tessera::compiler::ir
