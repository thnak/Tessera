// SPDX-License-Identifier: MIT

// Expressions, actions, source spans and nodes to JSON.

#include "ir_json_internal.hpp"
#include "tessera/compiler/ir/ir_json.hpp"

#include <utility>

namespace tessera::compiler::ir {
namespace {

JsonValue literal_to_json(const LiteralValue& literal) {
    if (const auto* flag = std::get_if<bool>(&literal)) {
        return JsonValue::boolean(*flag);
    }
    if (const auto* integer = std::get_if<std::int64_t>(&literal)) {
        return JsonValue::integer(*integer);
    }
    if (const auto* number = std::get_if<double>(&literal)) {
        return JsonValue::number(*number);
    }
    if (const auto* text = std::get_if<std::string>(&literal)) {
        return JsonValue::string(*text);
    }
    return JsonValue{};
}

const Expr* operand(const Expr& expr, std::size_t index) {
    return index < expr.operands.size() ? expr.operands[index].get() : nullptr;
}

} // namespace

JsonValue expr_to_json(const Expr* expr) {
    if (expr == nullptr) {
        return JsonValue{};
    }
    JsonValue json = JsonValue::object();
    switch (expr->kind) {
    case ExprKind::Literal:
        json.set("kind", JsonValue::string("literal"));
        json.set("value", literal_to_json(expr->literal));
        break;
    case ExprKind::Ref:
        json.set("kind", JsonValue::string("ref"));
        json.set("id", JsonValue::string(expr->id));
        break;
    case ExprKind::Binary:
        json.set("kind", JsonValue::string("binary"));
        json.set("op", JsonValue::string(expr->op));
        json.set("left", expr_to_json(operand(*expr, 0)));
        json.set("right", expr_to_json(operand(*expr, 1)));
        break;
    case ExprKind::Unary:
        json.set("kind", JsonValue::string("unary"));
        json.set("op", JsonValue::string(expr->op));
        json.set("value", expr_to_json(operand(*expr, 0)));
        break;
    case ExprKind::Conditional:
        json.set("kind", JsonValue::string("conditional"));
        json.set("test", expr_to_json(operand(*expr, 0)));
        json.set("then", expr_to_json(operand(*expr, 1)));
        json.set("else", expr_to_json(operand(*expr, 2)));
        break;
    case ExprKind::Template: {
        json.set("kind", JsonValue::string("template"));
        JsonValue parts = JsonValue::array();
        for (const auto& part : expr->parts) {
            parts.push(part.expr ? expr_to_json(part.expr.get()) : JsonValue::string(part.text));
        }
        json.set("parts", std::move(parts));
        break;
    }
    case ExprKind::Call: {
        json.set("kind", JsonValue::string("call"));
        json.set("callee", JsonValue::string(expr->id));
        JsonValue args = JsonValue::array();
        for (const auto& arg : expr->operands) {
            args.push(expr_to_json(arg.get()));
        }
        json.set("args", std::move(args));
        break;
    }
    }
    return json;
}

JsonValue value_to_json(const Expr* expr) {
    if (expr != nullptr && expr->kind == ExprKind::Literal) {
        return literal_to_json(expr->literal);
    }
    return expr_to_json(expr);
}

JsonValue to_json(const SourceSpan& span) {
    JsonValue json = JsonValue::object();
    json.set("file", JsonValue::string(span.file));
    json.set("offset", JsonValue::integer(span.offset));
    json.set("length", JsonValue::integer(span.length));
    json.set("line", JsonValue::integer(span.line));
    json.set("column", JsonValue::integer(span.column));
    json.set("endLine", JsonValue::integer(span.end_line));
    json.set("endColumn", JsonValue::integer(span.end_column));
    return json;
}

JsonValue actions_to_json(const std::vector<Action>& actions) {
    JsonValue list = JsonValue::array();
    for (const auto& action : actions) {
        JsonValue json = JsonValue::object();
        json.set("kind", JsonValue::string(action.kind == ActionKind::Set ? "set" : "animSet"));
        json.set("target", JsonValue::string(action.target));
        json.set("value", expr_to_json(action.value.get()));
        if (action.kind == ActionKind::AnimSet && !action.animate) {
            json.set("animate", JsonValue::boolean(false));
        }
        list.push(std::move(json));
    }
    return list;
}

JsonValue params_to_json(const std::vector<Param>& params) {
    JsonValue list = JsonValue::array();
    for (const auto& param : params) {
        JsonValue json = JsonValue::object();
        json.set("name", JsonValue::string(param.name));
        json.set("type", JsonValue::string(param.type));
        list.push(std::move(json));
    }
    return list;
}

JsonValue to_json(const Node& node) {
    JsonValue json = JsonValue::object();
    json.set("kind", JsonValue::string(std::string(node_kind_name(node.kind))));
    json.set("id", JsonValue::string(node.id));
    json.set("uiId", JsonValue::string(node.ui_id));
    json.set("source", to_json(node.source));
    if (!node.automation_id.empty()) {
        json.set("automationId", JsonValue::string(node.automation_id));
    }
    if (!node.element.empty()) {
        json.set("element", JsonValue::string(node.element));
    }
    if (!node.component.empty()) {
        json.set("component", JsonValue::string(node.component));
    }
    if (node.key) {
        json.set("key", value_to_json(node.key.get()));
    }
    const auto write_map = [&](const char* name, const std::map<std::string, ExprPtr>& map) {
        if (map.empty()) {
            return;
        }
        JsonValue object = JsonValue::object();
        for (const auto& [key, value] : map) {
            object.set(key, value_to_json(value.get()));
        }
        json.set(name, std::move(object));
    };
    write_map("props", node.props);
    write_map("style", node.style);
    if (!node.events.empty()) {
        JsonValue events = JsonValue::object();
        for (const auto& [name, target] : node.events) {
            events.set(name, JsonValue::string(target));
        }
        json.set("events", std::move(events));
    }
    if (node.text) {
        json.set("text", expr_to_json(node.text.get()));
    }
    if (!node.children.empty()) {
        JsonValue children = JsonValue::array();
        for (const auto& child : node.children) {
            children.push(to_json(child));
        }
        json.set("children", std::move(children));
    }
    if (node.condition) {
        json.set("condition", expr_to_json(node.condition.get()));
    }
    if (node.loop) {
        JsonValue loop = JsonValue::object();
        loop.set("kind", JsonValue::string("loop"));
        loop.set("item", JsonValue::string(node.loop->item));
        if (!node.loop->index.empty()) {
            loop.set("index", JsonValue::string(node.loop->index));
        }
        loop.set("source", expr_to_json(node.loop->source.get()));
        json.set("loop", std::move(loop));
    }
    if (node.fallback) {
        json.set("fallback", to_json(*node.fallback));
    }
    return json;
}

} // namespace tessera::compiler::ir
