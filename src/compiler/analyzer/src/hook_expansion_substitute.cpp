// SPDX-License-Identifier: MIT

// Identifier substitution and renaming for the custom-hook expansion pass (spike #659, ADR-065;
// docs/specs/custom-hooks-v1.md section 4 steps 3 and 6).
//
// Both walk the whole tree, including embedded JSX, because a hook's returned value may be used in
// an attribute, a child expression or a `.map` callback. Arrow parameters shadow an outer name, so
// a handler parameter called `page` is not the hook's `page`.

#include "hook_expansion_internal.hpp"

#include <algorithm>
#include <utility>

namespace tessera::compiler::jsx::internal {
namespace {

// The value a `p.field` read resolves to, or null when `p` is not a whole-value binding or has no
// such field.
const ExprPtr* object_field(const Substitutions& subs, const Expr& member) {
    if (member.kind != ExprKind::Member || member.text == "[]" || member.operands.size() != 1) {
        return nullptr;
    }
    const Expr& object = *member.operands[0];
    if (object.kind != ExprKind::Identifier) {
        return nullptr;
    }
    const auto found = subs.objects.find(object.text);
    if (found == subs.objects.end()) {
        return nullptr;
    }
    const auto field = found->second.find(member.text);
    return field == found->second.end() ? nullptr : &field->second;
}

void substitute_children(Expr& expr, const Substitutions& subs,
                         std::vector<Span>& whole_value_uses) {
    for (ExprPtr& operand : expr.operands) {
        if (operand) {
            substitute(operand, subs, whole_value_uses);
        }
    }
    for (ObjectProperty& property : expr.properties) {
        if (property.value) {
            substitute(property.value, subs, whole_value_uses);
        }
    }
    if (expr.jsx) {
        substitute(*expr.jsx, subs, whole_value_uses);
    }
}

} // namespace

void substitute(ExprPtr& slot, const Substitutions& subs, std::vector<Span>& whole_value_uses) {
    if (!slot) {
        return;
    }
    // A whole-value binding read as `p.field` is the field's source (spec section 3).
    if (const ExprPtr* field = object_field(subs, *slot)) {
        const Span span = slot->span;
        slot = clone_expr(**field);
        slot->span = span;
        return;
    }
    if (slot->kind == ExprKind::Identifier) {
        const auto found = subs.values.find(slot->text);
        if (found != subs.values.end()) {
            const Span span = slot->span;
            slot = clone_expr(*found->second);
            slot->span = span;
            return;
        }
        if (subs.objects.count(slot->text) != 0) {
            // `p` alone is not a value: there is no record at run time (spec section 3).
            whole_value_uses.push_back(slot->span);
            return;
        }
    }
    substitute_children(*slot, subs, whole_value_uses);
}

void substitute(JsxNode& node, const Substitutions& subs, std::vector<Span>& whole_value_uses) {
    for (JsxAttribute& attribute : node.attributes) {
        if (attribute.expr) {
            substitute(attribute.expr, subs, whole_value_uses);
        }
    }
    if (node.expr) {
        substitute(node.expr, subs, whole_value_uses);
    }
    for (JsxPtr& child : node.children) {
        if (child) {
            substitute(*child, subs, whole_value_uses);
        }
    }
}

namespace {

void rename_children(Expr& expr, const std::map<std::string, std::string>& names) {
    for (ExprPtr& operand : expr.operands) {
        if (operand) {
            rename(*operand, names);
        }
    }
    for (ObjectProperty& property : expr.properties) {
        if (property.value) {
            rename(*property.value, names);
        }
    }
    if (expr.jsx) {
        rename(*expr.jsx, names);
    }
}

} // namespace

void rename(Expr& expr, const std::map<std::string, std::string>& names) {
    if (expr.kind == ExprKind::Identifier) {
        const auto found = names.find(expr.text);
        if (found != names.end()) {
            expr.text = found->second;
        }
        return;
    }
    if (expr.kind == ExprKind::Arrow) {
        // A parameter shadows the outer name for the whole body, so it is renamed to itself.
        std::map<std::string, std::string> inner = names;
        for (const std::string& param : expr.params) {
            inner.erase(param);
        }
        for (ExprPtr& statement : expr.operands) {
            if (statement) {
                rename(*statement, inner);
            }
        }
        return;
    }
    rename_children(expr, names);
}

void rename(JsxNode& node, const std::map<std::string, std::string>& names) {
    for (JsxAttribute& attribute : node.attributes) {
        if (attribute.expr) {
            rename(*attribute.expr, names);
        }
    }
    if (node.expr) {
        rename(*node.expr, names);
    }
    for (JsxPtr& child : node.children) {
        if (child) {
            rename(*child, names);
        }
    }
}

namespace {

void collect_from(const Expr& expr, std::vector<const Expr*>& out);
void collect_from(const JsxNode& node, std::vector<const Expr*>& out);

void collect_from(const Expr& expr, std::vector<const Expr*>& out) {
    if (expr.kind == ExprKind::Call && !expr.operands.empty() &&
        expr.operands[0]->kind == ExprKind::Identifier && expr.operands[0]->text == "useEffect" &&
        expr.operands.size() == 3 && expr.operands[2]->kind == ExprKind::Array) {
        out.push_back(expr.operands[2].get());
    }
    for (const ExprPtr& operand : expr.operands) {
        if (operand) {
            collect_from(*operand, out);
        }
    }
    for (const ObjectProperty& property : expr.properties) {
        if (property.value) {
            collect_from(*property.value, out);
        }
    }
    if (expr.jsx) {
        collect_from(*expr.jsx, out);
    }
}

void collect_from(const JsxNode& node, std::vector<const Expr*>& out) {
    for (const JsxAttribute& attribute : node.attributes) {
        if (attribute.expr) {
            collect_from(*attribute.expr, out);
        }
    }
    if (node.expr) {
        collect_from(*node.expr, out);
    }
    for (const JsxPtr& child : node.children) {
        if (child) {
            collect_from(*child, out);
        }
    }
}

} // namespace

void collect_dependency_arrays(const Expr& expr, std::vector<const Expr*>& out) {
    collect_from(expr, out);
}

} // namespace tessera::compiler::jsx::internal
