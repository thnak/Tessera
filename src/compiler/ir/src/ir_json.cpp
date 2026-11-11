// SPDX-License-Identifier: MIT

// Module, component and binding to JSON.

#include "ir_json_internal.hpp"
#include "tessera/compiler/ir/ir_json.hpp"

#include <utility>

namespace tessera::compiler::ir {
namespace {

JsonValue string_array(const std::vector<std::string>& values) {
    JsonValue list = JsonValue::array();
    for (const auto& value : values) {
        list.push(JsonValue::string(value));
    }
    return list;
}

void add_if_not_empty(JsonValue& json, const char* key, JsonValue list) {
    const auto* items = list.as_array();
    if (items != nullptr && !items->empty()) {
        json.set(key, std::move(list));
    }
}

JsonValue typed_entry(const std::string& id, const std::string& name, const std::string& type,
                      const Expr* initial) {
    JsonValue json = JsonValue::object();
    json.set("id", JsonValue::string(id));
    json.set("name", JsonValue::string(name));
    json.set("type", JsonValue::string(type));
    if (initial != nullptr) {
        json.set("initial", value_to_json(initial));
    }
    return json;
}

} // namespace

JsonValue to_json(const Component& component) {
    JsonValue json = JsonValue::object();
    json.set("id", JsonValue::string(component.id));
    json.set("componentId", JsonValue::string(component.component_id));
    json.set("name", JsonValue::string(component.name));
    json.set("source", to_json(component.source));
    json.set("root", to_json(component.root));

    JsonValue props = JsonValue::array();
    for (const auto& prop : component.props) {
        JsonValue entry = JsonValue::object();
        entry.set("name", JsonValue::string(prop.name));
        entry.set("type", JsonValue::string(prop.type));
        if (prop.optional) {
            entry.set("optional", JsonValue::boolean(true));
        }
        if (prop.default_value) {
            entry.set("default", value_to_json(prop.default_value.get()));
        }
        props.push(std::move(entry));
    }
    add_if_not_empty(json, "props", std::move(props));

    JsonValue state = JsonValue::array();
    for (const auto& item : component.state) {
        state.push(typed_entry(item.id, item.name, item.type, item.initial.get()));
    }
    add_if_not_empty(json, "state", std::move(state));

    JsonValue handlers = JsonValue::array();
    for (const auto& handler : component.handlers) {
        JsonValue entry = JsonValue::object();
        entry.set("id", JsonValue::string(handler.id));
        entry.set("source", to_json(handler.source));
        entry.set("actions", actions_to_json(handler.actions));
        add_if_not_empty(entry, "params", params_to_json(handler.params));
        handlers.push(std::move(entry));
    }
    add_if_not_empty(json, "handlers", std::move(handlers));

    JsonValue effects = JsonValue::array();
    for (const auto& effect : component.effects) {
        JsonValue entry = JsonValue::object();
        entry.set("id", JsonValue::string(effect.id));
        entry.set("deps", string_array(effect.deps));
        entry.set("handler", JsonValue::string(effect.handler));
        if (!effect.cleanup.empty()) {
            entry.set("cleanup", JsonValue::string(effect.cleanup));
        }
        entry.set("source", to_json(effect.source));
        effects.push(std::move(entry));
    }
    add_if_not_empty(json, "effects", std::move(effects));

    JsonValue refs = JsonValue::array();
    for (const auto& ref : component.refs) {
        JsonValue entry = JsonValue::object();
        entry.set("id", JsonValue::string(ref.id));
        entry.set("name", JsonValue::string(ref.name));
        entry.set("target", JsonValue::string(ref.target));
        refs.push(std::move(entry));
    }
    add_if_not_empty(json, "refs", std::move(refs));

    JsonValue animated = JsonValue::array();
    for (const auto& value : component.animated_values) {
        animated.push(typed_entry(value.id, value.name, value.type, value.initial.get()));
    }
    add_if_not_empty(json, "animatedValues", std::move(animated));

    JsonValue environment = JsonValue::array();
    for (const auto& env : component.environment) {
        JsonValue entry = JsonValue::object();
        entry.set("id", JsonValue::string(env.id));
        entry.set("name", JsonValue::string(env.name));
        entry.set("provider", JsonValue::string(env.provider));
        entry.set("type", JsonValue::string(env.type));
        if (env.writable) {
            entry.set("writable", JsonValue::boolean(true));
        }
        entry.set("source", to_json(env.source));
        environment.push(std::move(entry));
    }
    add_if_not_empty(json, "environment", std::move(environment));
    return json;
}

JsonValue to_json(const Binding& binding) {
    JsonValue json = JsonValue::object();
    json.set("id", JsonValue::string(binding.id));
    json.set("component", JsonValue::string(binding.component));
    json.set("kind", JsonValue::string(std::string(binding_kind_name(binding.kind))));
    json.set("source", to_json(binding.source));
    json.set("region", JsonValue::string(binding.region));
    add_if_not_empty(json, "params", params_to_json(binding.params));
    if (!binding.return_type.empty()) {
        json.set("returnType", JsonValue::string(binding.return_type));
    }
    if (binding.preview) {
        JsonValue preview = JsonValue::object();
        preview.set("actions", actions_to_json(binding.preview->actions));
        if (binding.preview->return_value) {
            preview.set("return", expr_to_json(binding.preview->return_value.get()));
        }
        json.set("preview", std::move(preview));
    }
    return json;
}

JsonValue to_json(const IrModule& module) {
    JsonValue json = JsonValue::object();
    json.set("irVersion", JsonValue::integer(module.ir_version));

    JsonValue info = JsonValue::object();
    info.set("name", JsonValue::string(module.name));
    add_if_not_empty(info, "imports", string_array(module.imports));
    json.set("module", std::move(info));

    JsonValue components = JsonValue::array();
    for (const auto& component : module.components) {
        components.push(to_json(component));
    }
    json.set("components", std::move(components));

    JsonValue bindings = JsonValue::array();
    for (const auto& binding : module.bindings) {
        bindings.push(to_json(binding));
    }
    json.set("bindings", std::move(bindings));

    JsonValue sheets = JsonValue::array();
    for (const auto& sheet : module.stylesheets) {
        JsonValue entry = JsonValue::object();
        entry.set("id", JsonValue::string(sheet.id));
        entry.set("path", JsonValue::string(sheet.path));
        entry.set("hash", JsonValue::string(sheet.hash));
        sheets.push(std::move(entry));
    }
    json.set("stylesheets", std::move(sheets));
    return json;
}

std::string write_ir_json(const IrModule& module) {
    return to_text(to_json(module));
}

} // namespace tessera::compiler::ir
