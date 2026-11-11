// SPDX-License-Identifier: MIT

// Slice 4 (partial): a component's declared prop records. The props parameter's type is either an
// inline object type or a same-file type alias whose target is an object; every member becomes one
// `ir::Prop` in declaration order (docs/specs/ir-v1.md section 2). Defaults, value-record types and
// callback *binding* stay documented gaps (docs/design/jsx-ir-lowering.md section 9).

#include "node_lowering_internal.hpp"
#include "tessera/compiler/jsx/parser.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tessera::compiler::lowering::internal {
namespace {

constexpr std::size_t kMaxAliasDepth = 8;

bool is_void_name(const jsx::TypeNode& type) {
    return type.kind == jsx::TypeKind::Name && type.children.empty() && type.text == "void";
}

bool is_null_name(const jsx::TypeNode& type) {
    return type.kind == jsx::TypeKind::Name && type.children.empty() &&
           (type.text == "null" || type.text == "undefined");
}

std::optional<std::string> lower_prop_type(const jsx::TypeNode& type);

// A list element of a value prop: the V1 list holds printable primitive values, never a slot,
// callback or nullable value (ir-v1 section 2).
std::optional<std::string> lower_list_element(const jsx::TypeNode& type) {
    const std::optional<std::string> element = lower_prop_type(type);
    if (!element || *element == "children" || *element == "callback" ||
        element->rfind("optional<", 0) == 0) {
        return std::nullopt;
    }
    return element;
}

// The written type -> IR v1 type mapping for one prop. `nullopt` means the V1 type list cannot
// represent it; the caller reports the prop and the source spelling, never drops it.
std::optional<std::string> lower_prop_type(const jsx::TypeNode& type) {
    switch (type.kind) {
    case jsx::TypeKind::Name:
        if (!type.children.empty() || type.text.empty()) {
            return std::nullopt; // a generic type (`Array<T>`, `Foo<T>`) has no V1 spelling
        }
        if (type.text == "Children" || type.text == "children") {
            return std::string("children");
        }
        if (type.text == "boolean" || type.text == "bool") {
            return std::string("bool");
        }
        if (type.text == "int" || type.text == "float" || type.text == "string" ||
            type.text == "color" || type.text == "length" || type.text == "node_ref") {
            return type.text;
        }
        if (type.text == "number" || type.text == "null" || type.text == "undefined" ||
            type.text == "void") {
            return std::nullopt;
        }
        // A payload record (`TextChangeEvent`) or an alias name: the IR validator accepts the
        // identifier until the type table exists, which is the existing rule for written types.
        return type.text;
    case jsx::TypeKind::StringLiteral:
        return std::string("string");
    case jsx::TypeKind::Array: {
        if (type.children.size() != 1 || !type.children[0]) {
            return std::nullopt;
        }
        const std::optional<std::string> element = lower_list_element(*type.children[0]);
        return element ? std::optional<std::string>("list<" + *element + ">") : std::nullopt;
    }
    case jsx::TypeKind::Union: {
        bool all_string_literals = !type.children.empty();
        for (const jsx::TypePtr& child : type.children) {
            all_string_literals =
                all_string_literals && child && child->kind == jsx::TypeKind::StringLiteral;
        }
        if (all_string_literals) {
            return std::string("string");
        }
        if (type.children.size() == 2 && type.children[0] && type.children[1]) {
            const jsx::TypeNode* inner = nullptr;
            if (is_null_name(*type.children[1])) {
                inner = type.children[0].get();
            } else if (is_null_name(*type.children[0])) {
                inner = type.children[1].get();
            }
            if (inner != nullptr) {
                const std::optional<std::string> value = lower_prop_type(*inner);
                if (!value) {
                    return std::nullopt;
                }
                return value->rfind("optional<", 0) == 0
                           ? value
                           : std::optional<std::string>("optional<" + *value + ">");
            }
        }
        return std::nullopt;
    }
    case jsx::TypeKind::Function: {
        // `() => void` is the payload-free callback spelling of a prop; `(e: Record) => void` is a
        // callback with one payload record (docs/specs/component-model-v1.md section 3). Any other
        // function shape needs the type table.
        if (type.children.size() == 1 && type.children[0] && is_void_name(*type.children[0])) {
            return std::string("callback");
        }
        if (type.children.size() == 2 && type.param_names.size() == 1 && type.children[0] &&
            type.children[1] && is_void_name(*type.children[1])) {
            const std::optional<std::string> payload = lower_prop_type(*type.children[0]);
            if (!payload || *payload == "children" || *payload == "callback" ||
                payload->rfind("optional<", 0) == 0) {
                return std::nullopt;
            }
            return payload;
        }
        return std::nullopt;
    }
    case jsx::TypeKind::Object:
        return std::nullopt; // an inline value record has no V1 IR type
    }
    return std::nullopt;
}

const jsx::ModuleItem* find_alias(const jsx::Module& module, std::string_view name) {
    for (const jsx::ModuleItem& item : module.items) {
        if (item.kind == jsx::ItemKind::TypeAlias && item.name == name) {
            return &item;
        }
    }
    return nullptr;
}

// The declared members of the props type, or null when the component declares no props parameter,
// or its type is not an object (inline or through a same-file alias chain). An imported alias is a
// documented gap and stays unlowered rather than becoming an error.
const std::vector<jsx::TypeMember>* props_members(const NodeState& state) {
    const jsx::ModuleItem& component = state.context.component;
    if (!component.has_props || !component.props_type) {
        return nullptr;
    }
    const jsx::TypeNode* type = component.props_type.get();
    for (std::size_t depth = 0; depth < kMaxAliasDepth && type != nullptr; ++depth) {
        if (type->kind == jsx::TypeKind::Object) {
            return &type->members;
        }
        if (type->kind != jsx::TypeKind::Name || !type->children.empty() ||
            state.context.module == nullptr) {
            return nullptr;
        }
        const jsx::ModuleItem* alias = find_alias(*state.context.module, type->text);
        if (alias == nullptr || !alias->type) {
            return nullptr;
        }
        type = alias->type.get();
    }
    return nullptr;
}

} // namespace

DeclaredPropsKind lower_declared_props(NodeState& state, std::vector<ir::Prop>& props) {
    const std::vector<jsx::TypeMember>* members = props_members(state);
    if (members == nullptr) {
        return DeclaredPropsKind::None;
    }
    std::unordered_set<std::string> names;
    for (const jsx::TypeMember& member : *members) {
        if (member.name.empty() || !member.type) {
            fail_node(state, "TESSERA_IR_MALFORMED",
                      "a declared prop record needs a name and a type", member.span);
            return DeclaredPropsKind::Error;
        }
        if (!names.insert(member.name).second) {
            fail_node(state, "TESSERA_IR_DUPLICATE_ID", "duplicate prop `" + member.name + "`",
                      member.span);
            return DeclaredPropsKind::Error;
        }
        const std::optional<std::string> type = lower_prop_type(*member.type);
        if (!type) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED",
                      "prop `" + member.name + "` has a type with no IR form in v1 (`" +
                          jsx::dump(*member.type) + "`)",
                      member.type->span,
                      "write an IR type, for example `string`, `int`, `T[]`, `optional<...>` or "
                      "a payload record name");
            return DeclaredPropsKind::Error;
        }
        if (member.name == "children") {
            // Section 3.4: the slot has the `children` type, no default and no `?`.
            if (member.optional || *type != "children") {
                fail_node(state, "TESSERA_IR_SLOT",
                          "the `children` prop declares the static slot, so it has type `Children` "
                          "and no `?`",
                          member.span);
                return DeclaredPropsKind::Error;
            }
            props.push_back(ir::Prop{"children", "children", nullptr});
            continue;
        }
        if (*type == "children") {
            fail_node(state, "TESSERA_IR_SLOT",
                      "prop `" + member.name + "` cannot have the `children` slot type",
                      member.span);
            return DeclaredPropsKind::Error;
        }
        props.push_back(ir::Prop{member.name, *type, nullptr, member.optional});
    }
    return DeclaredPropsKind::Lowered;
}

} // namespace tessera::compiler::lowering::internal
