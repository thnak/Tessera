// SPDX-License-Identifier: MIT

// Component prop and callback-prop instantiation (docs/design/compiler.md section 11). A library
// component such as `Field` is an ordinary declared component here: its props are the declared prop
// records, its callback prop is a record-typed prop bound through the node's events, and its
// children ride the static `Children` slot. There is no Field-specific branch.

#include "emit_events.hpp"
#include "emit_props.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tessera::compiler::codegen::internal {
namespace {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

const ir::Prop* find_declared_prop(const ir::Component& target, std::string_view name) {
    for (const ir::Prop& prop : target.props) {
        if (prop.name == name) {
            return &prop;
        }
    }
    return nullptr;
}

const ir::Handler* find_handler(const ir::Component& owner, std::string_view id) {
    for (const ir::Handler& handler : owner.handlers) {
        if (handler.id == id) {
            return &handler;
        }
    }
    return nullptr;
}

const ir::Binding* find_binding(const ir::IrModule& module, std::string_view id,
                                std::string_view component) {
    for (const ir::Binding& binding : module.bindings) {
        if (binding.id == id && binding.component == component) {
            return &binding;
        }
    }
    return nullptr;
}

// The element type of `list<T>`, or an empty view when `type` is not a list.
std::string_view list_element(std::string_view type) {
    if (type.size() > 6 && type.rfind("list<", 0) == 0 && type.back() == '>') {
        return type.substr(5, type.size() - 6);
    }
    return {};
}

std::string_view literal_kind(const ir::LiteralValue& literal) {
    if (std::holds_alternative<ir::Null>(literal)) {
        return "null";
    }
    if (std::holds_alternative<bool>(literal)) {
        return "boolean";
    }
    if (std::holds_alternative<std::int64_t>(literal)) {
        return "integer";
    }
    if (std::holds_alternative<double>(literal)) {
        return "float";
    }
    return "string";
}

// A literal prop value must fit the declared prop type. A non-literal expression is accepted: its
// type needs the type table (docs/specs/ir-v1.md section 2; slice 4).
bool literal_matches(std::string_view type, const ir::Expr& expr) {
    if (expr.kind != ir::ExprKind::Literal) {
        return true;
    }
    const ir::LiteralValue& literal = expr.literal;
    if (type == "bool") {
        return std::holds_alternative<bool>(literal);
    }
    if (type == "int") {
        return std::holds_alternative<std::int64_t>(literal);
    }
    if (type == "float" || type == "length") {
        return std::holds_alternative<std::int64_t>(literal) ||
               std::holds_alternative<double>(literal);
    }
    if (type == "string" || type == "color") {
        return std::holds_alternative<std::string>(literal);
    }
    return false;
}

bool prop_value_matches(std::string_view type, const ir::Expr& expr) {
    const std::string_view element = list_element(type);
    if (element.empty()) {
        return literal_matches(type, expr);
    }
    // A list prop takes an array literal (checked element-wise) or an expression of unknown type.
    if (expr.kind != ir::ExprKind::Array) {
        return expr.kind != ir::ExprKind::Literal;
    }
    for (const ir::ExprPtr& item : expr.operands) {
        if (item && !literal_matches(element, *item)) {
            return false;
        }
    }
    return true;
}

// Resolves the handler a callback prop names to the generated callable (`handler_1` or `on_pick`)
// and checks that it takes exactly the prop's payload record.
bool resolve_callback(const ir::IrModule& module, const ir::Component& owner, const ir::Prop& prop,
                      std::string_view target_id, std::string& callable, std::string& error) {
    std::vector<ir::Param> params;
    if (starts_with(target_id, "handler::")) {
        const ir::Handler* handler = find_handler(owner, target_id);
        if (handler == nullptr) {
            return fail(error, "callback prop `" + prop.name + "` names unknown handler `" +
                                   std::string(target_id) + "`");
        }
        callable = method_name(handler->id);
        params = handler->params;
    } else if (starts_with(target_id, "bind::")) {
        const ir::Binding* binding = find_binding(module, target_id, owner.id);
        if (binding == nullptr) {
            return fail(error, "callback prop `" + prop.name + "` names unknown binding `" +
                                   std::string(target_id) + "` of this component");
        }
        callable = binding->region;
        params = binding->params;
    } else {
        return fail(error, "callback prop `" + prop.name + "` must name a handler");
    }
    if (params.size() != 1 || params.front().type != prop.type) {
        return fail(error, "callback prop `" + prop.name + "` needs a handler taking one `" +
                               prop.type + "` parameter");
    }
    return true;
}

bool collect_value(const ir::Component& target, std::string_view name, const ir::Expr& value,
                   ComponentInstance& instance, std::string& error) {
    const ir::Prop* prop = find_declared_prop(target, name);
    if (prop == nullptr) {
        return fail(error,
                    "component `" + target.name + "` has no prop `" + std::string(name) + "`");
    }
    if (prop->type == "children") {
        return fail(error, "`children` of component `" + target.name +
                               "` is the slot; write the nodes between the tags");
    }
    if (!is_value_prop_type(prop->type)) {
        return fail(error, "callback prop `" + std::string(name) + "` of component `" +
                               target.name + "` is bound through the node's events, not a value");
    }
    if (!prop_value_matches(prop->type, value)) {
        std::string got = value.kind == ir::ExprKind::Array ? "array" : "literal";
        if (value.kind == ir::ExprKind::Literal) {
            got = std::string(literal_kind(value.literal)) + " literal";
        }
        return fail(error, "prop `" + std::string(name) + "` of component `" + target.name +
                               "` expects `" + prop->type + "`, got a " + got);
    }
    instance.props.push_back(PropBinding{prop->name, prop->type, &value, {}});
    return true;
}

bool collect_callback(const ir::IrModule& module, const ir::Component& owner,
                      const ir::Component& target, std::string_view name,
                      std::string_view target_id, ComponentInstance& instance, std::string& error) {
    const ir::Prop* prop = find_declared_prop(target, name);
    if (prop == nullptr) {
        return fail(error,
                    "component `" + target.name + "` has no prop `" + std::string(name) + "`");
    }
    if (is_value_prop_type(prop->type) || prop->type == "children") {
        return fail(error, "prop `" + std::string(name) + "` of component `" + target.name +
                               "` is not a callback prop");
    }
    if (callback_prop_type(prop->type).empty()) {
        return fail(error, "callback prop `" + std::string(name) + "` of component `" +
                               target.name + "` has type `" + prop->type +
                               "` with no scene record mapping");
    }
    std::string callable;
    if (!resolve_callback(module, owner, *prop, target_id, callable, error)) {
        return false;
    }
    instance.props.push_back(PropBinding{prop->name, prop->type, nullptr, std::move(callable)});
    return true;
}

} // namespace

bool is_value_prop_type(std::string_view ir_type) {
    if (ir_type == "bool" || ir_type == "int" || ir_type == "float" || ir_type == "string" ||
        ir_type == "color" || ir_type == "length" || ir_type == "node_ref") {
        return true;
    }
    for (const std::string_view wrapper : {"list<", "optional<"}) {
        if (ir_type.size() > wrapper.size() + 1 && ir_type.rfind(wrapper, 0) == 0 &&
            ir_type.back() == '>') {
            return is_value_prop_type(
                ir_type.substr(wrapper.size(), ir_type.size() - wrapper.size() - 1));
        }
    }
    return false;
}

std::string callback_prop_type(std::string_view ir_type) {
    const PayloadRecord* payload = find_payload(ir_type);
    if (payload == nullptr) {
        return {};
    }
    return "std::function<void(const tessera::ui::" + std::string(payload->cpp_type) + "&)>";
}

bool collect_instance_props(const ir::IrModule& module, const ir::Component& owner,
                            const ir::Node& node, const ir::Component& target,
                            ComponentInstance& instance, std::string& error) {
    // Declared prop order first, so the generated assignments do not depend on the source order of
    // the attributes.
    for (const ir::Prop& prop : target.props) {
        const auto found = node.props.find(prop.name);
        if (found != node.props.end() && found->second) {
            if (!collect_value(target, prop.name, *found->second, instance, error)) {
                return false;
            }
        }
    }
    for (const auto& [name, value] : node.props) {
        if (find_declared_prop(target, name) == nullptr) {
            return fail(error, "component `" + target.name + "` has no prop `" + name + "`");
        }
    }
    for (const auto& [name, target_id] : node.events) {
        if (!collect_callback(module, owner, target, name, target_id, instance, error)) {
            return false;
        }
    }
    return true;
}

bool emit_apply_props(CppWriter& writer, const ir::Component& owner,
                      const std::vector<ComponentInstance>& instances, std::string& error) {
    bool any = false;
    for (const ComponentInstance& instance : instances) {
        any = any || !instance.props.empty();
    }
    if (!any) {
        return true;
    }
    writer.open("void " + owner.name + "::apply_props()");
    for (const ComponentInstance& instance : instances) {
        if (instance.props.empty()) {
            continue;
        }
        writer.line("// " + instance.component_id + " instance (pre-order index " +
                    std::to_string(instance.ordinal) + ") props.");
        const std::string member_prefix = "this->" + instance_member(instance.ordinal) + ".";
        for (const PropBinding& binding : instance.props) {
            const std::string member = member_prefix + member_name("prop::" + binding.name);
            if (binding.is_callback()) {
                const PayloadRecord* payload = find_payload(binding.type);
                writer.line(member + " = [this](const tessera::ui::" +
                            std::string(payload->cpp_type) + "& event) {");
                ++writer.indent;
                writer.line("this->" + binding.callback + "(event);");
                --writer.indent;
                writer.line("};");
                continue;
            }
            ExprEmission value = emit_initializer_expr(*binding.value, owner);
            if (!value.error.empty()) {
                return fail(error, value.error);
            }
            writer.line(member + " = " + value.text + ";");
        }
    }
    writer.close("");
    writer.line("");
    return true;
}

} // namespace tessera::compiler::codegen::internal
