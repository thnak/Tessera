// SPDX-License-Identifier: MIT

// tessera.d.ts (docs/specs/jsx-intrinsics-v1/machine-readable.md section 5, "Typings"): one line
// per generated declaration. The tool is the only writer; the golden test compares the bytes.

#include "core.hpp"
#include "tessera/schema/intrinsics.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace tessera::schema::dump {
namespace {

std::string ts_type(std::string_view type) {
    if (type == "bool") {
        return "boolean";
    }
    if (type == "int" || type == "float" || type == "number") {
        return "number";
    }
    if (type == "string") {
        return "string";
    }
    if (type == "length") {
        return "number | string";
    }
    if (type == "color") {
        return "string";
    }
    if (type == "one element") {
        return "JSX.Element";
    }
    if (type == "object literal") {
        return "Record<string, unknown>";
    }
    if (type == "enum") {
        return "string";
    }
    if (type == "string | int") {
        return "string | number";
    }
    if (type == "string | int | bool") {
        return "string | number | boolean";
    }
    if (type == "string literal, decimal uint64") {
        return "string";
    }
    if (type == "color | var(--name) | \"none\"") {
        return "string";
    }
    if (!type.empty() && type.front() == '"') {
        return std::string(type); // an enum union of string literals is already TypeScript
    }
    return std::string(type);
}

const PropRow* find_prop(const ElementRow& element, std::string_view name) {
    for (const PropRow& prop : element.props) {
        if (prop.name == name) {
            return &prop;
        }
    }
    return nullptr;
}

// The opaque handle type of an element (`ViewElement`, `CustomElement`).
std::string element_handle(std::string_view ref_type) {
    std::string handle(ref_type);
    constexpr std::string_view kSuffix = "Element";
    if (handle.size() < kSuffix.size() ||
        handle.compare(handle.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
        handle += "Element";
    }
    return handle;
}

void append_common_props(std::string& out) {
    out += "export interface CommonProps {\n";
    out += "  key?: string | number;\n";
    out += "  className?: string;\n";
    out += "  style?: Record<string, string | number>;\n";
    out += "  id?: string;\n";
    out += "  role?: RoleToken;\n";
    for (const PropRow& prop : common_props()) {
        if (prop.name == "key" || prop.name == "className" || prop.name == "style" ||
            prop.name == "id" || prop.name == "role" || prop.name == "ref") {
            continue;
        }
        out += "  '" + std::string(prop.name) + "'?: " + ts_type(prop.type) + ";\n";
    }
    out += "}\n\n";
}

void append_element_interface(std::string& out, const ElementRow& element) {
    out += "export interface " + std::string(element.tag) + "Props extends CommonProps {\n";
    if (!element.ref_type.empty()) {
        out += "  ref?: ElementRef<" + element_handle(element.ref_type) + ">;\n";
    }
    for (const PropRow& prop : element.props) {
        // A row restricted to other elements (rows/wrap on TextArea, secure on TextInput) is not
        // part of this interface.
        if (!prop.only_on.empty() && std::find(prop.only_on.begin(), prop.only_on.end(),
                                               element.tag) == prop.only_on.end()) {
            continue;
        }
        out += "  " + std::string(prop.name) + (prop.required ? ": " : "?: ") + ts_type(prop.type) +
               ";\n";
    }
    for (const EventRow& event : element.events) {
        const std::string payload = event.payload.empty() ? "unknown" : std::string(event.payload);
        out += "  " + std::string(event.name) + "?: (e: " + payload + ") => void;\n";
    }
    out += "}\n\n";
}

void append_alias_interface(std::string& out, const AliasRow& alias) {
    const ElementRow* target = find_element(alias.target);
    if (target == nullptr) {
        return;
    }
    out += "export interface " + std::string(alias.tag) + "Props extends " +
           std::string(target->tag) + "Props {\n";
    if (!alias.type_map.empty()) {
        out += "  type?: ";
        bool first = true;
        if (alias.type_map_closed) {
            out += "\"text\"";
            first = false;
        }
        for (const AliasTypeMap& entry : alias.type_map) {
            out += (first ? "" : " | ") + ("\"" + std::string(entry.type) + "\"");
            first = false;
        }
        out += ";\n";
    }
    for (const AliasAttr& attr : alias.attrs) {
        switch (attr.kind) {
        case AliasAttrKind::Rename: {
            const PropRow* source = find_prop(*target, attr.rename);
            out += "  " + std::string(attr.name) +
                   "?: " + (source != nullptr ? ts_type(source->type) : std::string("string")) +
                   ";\n";
            break;
        }
        case AliasAttrKind::ToStyle:
            out += "  " + std::string(attr.name) + "?: number | string;\n";
            break;
        case AliasAttrKind::ValueMap: {
            out += "  " + std::string(attr.name) + "?: ";
            bool first = true;
            for (const AliasValue& value : attr.values) {
                out += (first ? "" : " | ") + ("\"" + std::string(value.value) + "\"");
                first = false;
            }
            out += ";\n";
            break;
        }
        case AliasAttrKind::Keep:
        case AliasAttrKind::Reject:
            break;
        }
    }
    out += "}\n\n";
}

} // namespace

std::string dts() {
    std::string out;
    out += "// Generated by tessera_schema_dump from the shared schema (schemaVersion " +
           std::to_string(kSchemaVersion) + "). Do not edit.\n";
    out += "// Update with: tessera_schema_dump --write tests/golden/schema\n\n";
    out += "export type int = number;\n";
    out += "export type float = number;\n";
    out += "export type bool = boolean;\n";
    out += "export type length = number | string;\n";
    out += "export type color = string;\n";
    out += "export interface ElementRef<T> {\n  readonly current: T | null;\n}\n\n";

    out += "export type RoleToken = ";
    bool first = true;
    for (const std::string_view token : role_tokens()) {
        out += (first ? "" : " | ") + ("\"" + std::string(token) + "\"");
        first = false;
    }
    out += ";\n\n";

    for (const ElementRow& element : elements()) {
        if (!element.ref_type.empty()) {
            out += "export interface " + element_handle(element.ref_type) +
                   " {\n  readonly kind: \"" + std::string(element.tag) + "\";\n}\n\n";
        }
    }

    for (const TypeRecord& record : type_records()) {
        out += "export interface " + std::string(record.name) + " {\n";
        for (const TypeField& field : record.fields) {
            out += "  " + std::string(field.name) + ": " + ts_type(field.type) + ";\n";
        }
        out += "}\n\n";
    }

    append_common_props(out);
    for (const ElementRow& element : elements()) {
        append_element_interface(out, element);
    }
    for (const AliasRow& alias : aliases()) {
        append_alias_interface(out, alias);
    }

    for (const DomType& type : dom_types()) {
        out +=
            "export type " + std::string(type.name) + " = " + element_handle(type.target) + ";\n";
    }
    out += "\n";

    out += "declare global {\n  namespace JSX {\n    interface IntrinsicElements {\n";
    for (const ElementRow& element : elements()) {
        out += "      " + std::string(element.tag) + ": " + std::string(element.tag) + "Props;\n";
    }
    for (const AliasRow& alias : aliases()) {
        if (alias.status != AliasStatus::Unsupported) {
            out += "      " + std::string(alias.tag) + ": " + std::string(alias.tag) + "Props;\n";
        }
    }
    out += "    }\n  }\n}\n";
    return out;
}

} // namespace tessera::schema::dump
