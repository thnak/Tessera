// SPDX-License-Identifier: MIT

// Node tree checks (docs/specs/ir-v1.md sections 3, 3.1, 3.2, 3.3, 3.4, 5 and 5.1).

#include "validate_internal.hpp"

#include <string_view>
#include <utility>

namespace tessera::compiler::ir {
namespace {

constexpr int kMaxLoopDepth = 2; // section 5

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

struct NodeWalker {
    ValidationContext& context;
    ComponentScope& scope;

    void walk(const Node& node, const LocalNames& outer, int loop_depth);
    void check_identity(const Node& node);
    void check_shape(const Node& node);
    void check_events(const Node& node);
    void check_loop(const Node& node, const LocalNames& outer, int loop_depth);
};

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
    if (!component && !node.component.empty()) {
        context.report("TESSERA_IR_MALFORMED", "only a Component node has `component`",
                       node.source);
    }
    const bool leaf =
        custom || component || node.kind == NodeKind::TextInput || node.kind == NodeKind::TextArea;
    if (leaf && !node.children.empty()) {
        context.report("TESSERA_IR_LEAF",
                       std::string(node_kind_name(node.kind)) + " must not have children",
                       node.source);
    }
    if (node.kind == NodeKind::ErrorBoundary) {
        if (!node.fallback) {
            context.report("TESSERA_IR_ERROR_BOUNDARY", "an ErrorBoundary needs a fallback",
                           node.source);
        }
        if (node.children.size() != 1) {
            context.report("TESSERA_IR_ERROR_BOUNDARY",
                           "an ErrorBoundary needs exactly one child subtree", node.source);
        }
    } else if (node.fallback) {
        context.report("TESSERA_IR_MALFORMED", "only an ErrorBoundary has a fallback", node.source);
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

void NodeWalker::walk(const Node& node, const LocalNames& outer, int loop_depth) {
    check_identity(node);
    check_shape(node);
    check_events(node);

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
    validate_expr(context, scope, locals, node.condition.get(), node.source);
    for (const auto& child : node.children) {
        walk(child, locals, loop_depth);
    }
    if (node.fallback) {
        walk(*node.fallback, locals, loop_depth);
    }
}

} // namespace

void validate_node_tree(ValidationContext& context, ComponentScope& scope, const Node& root) {
    NodeWalker walker{context, scope};
    walker.walk(root, {}, 0);
}

} // namespace tessera::compiler::ir
