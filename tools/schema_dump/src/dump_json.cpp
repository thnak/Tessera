// SPDX-License-Identifier: MIT

// intrinsics.json (docs/specs/jsx-intrinsics-v1/machine-readable.md sections 3 and 4). It reuses
// the IR JSON writer of `tessera_ir` so the bytes follow the same rules: keys in byte order, two
// space indent, LF, one trailing newline, no locale.

#include "core.hpp"
#include "tessera/compiler/ir/json_value.hpp"
#include "tessera/schema/intrinsics.hpp"
#include "tessera/schema/style.hpp"

#include <span>
#include <string>
#include <string_view>

namespace tessera::schema::dump {
namespace {

namespace ir = tessera::compiler::ir;

using ir::JsonValue;

JsonValue string_array(std::span<const std::string_view> values) {
    JsonValue array = JsonValue::array();
    for (const std::string_view value : values) {
        array.push(JsonValue::string(std::string(value)));
    }
    return array;
}

JsonValue prop_json(const PropRow& prop) {
    JsonValue json = JsonValue::object();
    json.set("name", JsonValue::string(std::string(prop.name)));
    json.set("type", JsonValue::string(std::string(prop.type)));
    json.set("value", JsonValue::string(std::string(to_string(prop.value))));
    if (prop.required) {
        json.set("required", JsonValue::boolean(true));
    }
    if (!prop.default_value.empty()) {
        json.set("default", JsonValue::string(std::string(prop.default_value)));
    }
    json.set("lowersTo", JsonValue::string(std::string(prop.lowers_to)));
    if (!prop.only_on.empty()) {
        json.set("onlyOn", string_array(prop.only_on));
    }
    if (!prop.not_on.empty()) {
        json.set("notOn", string_array(prop.not_on));
    }
    if (!prop.note.empty()) {
        json.set("note", JsonValue::string(std::string(prop.note)));
    }
    return json;
}

JsonValue event_json(const EventRow& event) {
    JsonValue json = JsonValue::object();
    json.set("name", JsonValue::string(std::string(event.name)));
    if (!event.payload.empty()) {
        json.set("payload", JsonValue::string(std::string(event.payload)));
    }
    if (!event.note.empty()) {
        json.set("note", JsonValue::string(std::string(event.note)));
    }
    return json;
}

JsonValue constraint_json(const ConstraintRow& constraint) {
    JsonValue json = JsonValue::object();
    json.set("kind", JsonValue::string(std::string(to_string(constraint.kind))));
    if (!constraint.props.empty()) {
        json.set("props", string_array(constraint.props));
    }
    if (constraint.or_children) {
        json.set("orChildren", JsonValue::boolean(true));
    }
    json.set("code", JsonValue::string(std::string(constraint.code)));
    if (!constraint.note.empty()) {
        json.set("note", JsonValue::string(std::string(constraint.note)));
    }
    return json;
}

JsonValue inject_json(std::span<const AliasInject> inject) {
    JsonValue json = JsonValue::array();
    for (const AliasInject& entry : inject) {
        JsonValue item = JsonValue::object();
        item.set("name", JsonValue::string(std::string(entry.name)));
        item.set("value", JsonValue::string(std::string(entry.value)));
        json.push(std::move(item));
    }
    return json;
}

JsonValue value_json(const AliasValue& value) {
    JsonValue json = JsonValue::object();
    json.set("value", JsonValue::string(std::string(value.value)));
    if (value.rejected) {
        json.set("rejected", JsonValue::boolean(true));
    }
    if (!value.replacement.empty()) {
        json.set("replacement", JsonValue::string(std::string(value.replacement)));
    }
    if (!value.suggestion.empty()) {
        json.set("suggestion", JsonValue::string(std::string(value.suggestion)));
    }
    return json;
}

JsonValue attr_json(const AliasAttr& attr) {
    JsonValue json = JsonValue::object();
    json.set("name", JsonValue::string(std::string(attr.name)));
    json.set("kind", JsonValue::string(std::string(to_string(attr.kind))));
    if (!attr.rename.empty()) {
        json.set("rename", JsonValue::string(std::string(attr.rename)));
    }
    if (!attr.suggestion.empty()) {
        json.set("suggestion", JsonValue::string(std::string(attr.suggestion)));
    }
    if (!attr.values.empty()) {
        JsonValue values = JsonValue::array();
        for (const AliasValue& value : attr.values) {
            values.push(value_json(value));
        }
        json.set("values", std::move(values));
    }
    if (!attr.note.empty()) {
        json.set("note", JsonValue::string(std::string(attr.note)));
    }
    return json;
}

JsonValue alias_json(const AliasRow& alias) {
    JsonValue json = JsonValue::object();
    json.set("tag", JsonValue::string(std::string(alias.tag)));
    if (!alias.target.empty()) {
        json.set("target", JsonValue::string(std::string(alias.target)));
    }
    json.set("status", JsonValue::string(std::string(to_string(alias.status))));
    if (!alias.inject.empty()) {
        json.set("inject", inject_json(alias.inject));
    }
    if (!alias.attrs.empty()) {
        JsonValue attrs = JsonValue::array();
        for (const AliasAttr& attr : alias.attrs) {
            attrs.push(attr_json(attr));
        }
        json.set("attrs", std::move(attrs));
    }
    if (!alias.type_map.empty()) {
        JsonValue map = JsonValue::object();
        for (const AliasTypeMap& entry : alias.type_map) {
            JsonValue injected = JsonValue::object();
            for (const AliasInject& inject : entry.inject) {
                injected.set(std::string(inject.name),
                             JsonValue::string(std::string(inject.value)));
            }
            map.set(std::string(entry.type), std::move(injected));
        }
        json.set("typeMap", std::move(map));
    }
    if (alias.type_map_closed) {
        json.set("typeMapClosed", JsonValue::boolean(true));
    }
    if (!alias.alternative.empty()) {
        json.set("alternative", JsonValue::string(std::string(alias.alternative)));
    }
    if (!alias.note.empty()) {
        json.set("note", JsonValue::string(std::string(alias.note)));
    }
    return json;
}

JsonValue element_json(const ElementRow& element) {
    JsonValue json = JsonValue::object();
    json.set("tag", JsonValue::string(std::string(element.tag)));
    json.set("irKind", JsonValue::string(std::string(element.ir_kind)));
    json.set("children", JsonValue::string(std::string(to_string(element.children))));
    if (!element.ref_type.empty()) {
        json.set("refType", JsonValue::string(std::string(element.ref_type)));
    }
    if (!element.implied_role.empty()) {
        json.set("impliedRole", JsonValue::string(std::string(element.implied_role)));
    }
    json.set("acceptsClassName", JsonValue::boolean(element.accepts_class_name));
    json.set("acceptsStyle", JsonValue::boolean(element.accepts_style));
    JsonValue props = JsonValue::array();
    for (const PropRow& prop : element.props) {
        props.push(prop_json(prop));
    }
    json.set("props", std::move(props));
    JsonValue events = JsonValue::array();
    for (const EventRow& event : element.events) {
        events.push(event_json(event));
    }
    json.set("events", std::move(events));
    JsonValue constraints = JsonValue::array();
    for (const ConstraintRow& constraint : element.constraints) {
        constraints.push(constraint_json(constraint));
    }
    json.set("constraints", std::move(constraints));
    if (!element.note.empty()) {
        json.set("note", JsonValue::string(std::string(element.note)));
    }
    return json;
}

JsonValue type_json(const TypeRecord& record) {
    JsonValue json = JsonValue::object();
    json.set("name", JsonValue::string(std::string(record.name)));
    JsonValue fields = JsonValue::array();
    for (const TypeField& field : record.fields) {
        JsonValue item = JsonValue::object();
        item.set("name", JsonValue::string(std::string(field.name)));
        item.set("type", JsonValue::string(std::string(field.type)));
        fields.push(std::move(item));
    }
    json.set("fields", std::move(fields));
    if (!record.primary.empty()) {
        json.set("primary", JsonValue::string(std::string(record.primary)));
    }
    if (!record.note.empty()) {
        json.set("note", JsonValue::string(std::string(record.note)));
    }
    return json;
}

} // namespace

std::string json() {
    JsonValue root = JsonValue::object();
    root.set("schemaVersion", JsonValue::integer(kSchemaVersion));

    JsonValue types = JsonValue::array();
    for (const TypeRecord& record : type_records()) {
        types.push(type_json(record));
    }
    root.set("types", std::move(types));

    JsonValue common = JsonValue::array();
    for (const PropRow& prop : common_props()) {
        common.push(prop_json(prop));
    }
    root.set("common", std::move(common));

    JsonValue role_tokens_json = JsonValue::array();
    for (const std::string_view token : role_tokens()) {
        role_tokens_json.push(JsonValue::string(std::string(token)));
    }
    root.set("roleTokens", std::move(role_tokens_json));

    JsonValue elements_json = JsonValue::array();
    for (const ElementRow& element : elements()) {
        elements_json.push(element_json(element));
    }
    root.set("elements", std::move(elements_json));

    JsonValue aliases_json = JsonValue::array();
    for (const AliasRow& alias : aliases()) {
        aliases_json.push(alias_json(alias));
    }
    root.set("aliases", std::move(aliases_json));

    JsonValue dom_types_json = JsonValue::array();
    for (const DomType& type : dom_types()) {
        JsonValue item = JsonValue::object();
        item.set("name", JsonValue::string(std::string(type.name)));
        item.set("target", JsonValue::string(std::string(type.target)));
        dom_types_json.push(std::move(item));
    }
    root.set("domTypes", std::move(dom_types_json));

    JsonValue style = JsonValue::array();
    for (const StyleProperty& property : style_properties()) {
        JsonValue item = JsonValue::object();
        item.set("name", JsonValue::string(std::string(property.name)));
        item.set("kind", JsonValue::string(std::string(to_string(property.kind))));
        if (!property.note.empty()) {
            item.set("note", JsonValue::string(std::string(property.note)));
        }
        style.push(std::move(item));
    }
    root.set("style", std::move(style));

    return ir::to_text(root);
}

} // namespace tessera::schema::dump
