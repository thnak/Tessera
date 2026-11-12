// SPDX-License-Identifier: MIT

// Slice 4: a component's declared prop records. The props parameter's type is either an inline
// object type or a type alias; the alias may be declared in another project file and reached
// through `import type`/`import`, an `as` rename or a re-export chain (issue #70). Every member
// becomes one `ir::Prop` in declaration order (docs/specs/ir-v1.md section 2), so where the alias
// is authored cannot change the records. A destructuring default (`{ label = "" }`) becomes the
// record's `default_value`; value-record types and callback *binding* stay documented gaps
// (docs/design/jsx-ir-lowering.md section 9).

#include "node_lowering_internal.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/type_alias.hpp"

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

// The default forms of jsx-subset-v1.md section 6: a literal, or a flat list of literals. A
// computed default would need a runtime, and the IR has no expression form for a list of lists.
bool is_literal_default(const jsx::Expr& expr) {
    switch (expr.kind) {
    case jsx::ExprKind::Number:
    case jsx::ExprKind::String:
    case jsx::ExprKind::Boolean:
    case jsx::ExprKind::Null:
        return true;
    case jsx::ExprKind::Array:
        for (const jsx::ExprPtr& element : expr.operands) {
            if (!element || element->kind == jsx::ExprKind::Array ||
                !is_literal_default(*element)) {
                return false;
            }
        }
        return true;
    default:
        return false;
    }
}

const jsx::PropBinding* find_binding(const NodeState& state, std::string_view name) {
    for (const jsx::PropBinding& binding : state.context.component.props_bindings) {
        if (binding.name == name) {
            return &binding;
        }
    }
    return nullptr;
}

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

// The object body of the props type, and the file its member spans belong to.
struct MembersView {
    const std::vector<jsx::TypeMember>* members = nullptr;
    std::string_view file;
};

// Reports a type-resolution error. The span may live in another project file; `TypeNameTable`
// carries that file's source map so the diagnostic keeps the right line and column, and the message
// names the foreign file because `jsx::Diagnostic` has no file field.
void fail_type(NodeState& state, std::string_view code, std::string message, std::string_view file,
               jsx::Span span, std::string_view suggestion = {}) {
    if (state.error) {
        return;
    }
    const jsx::SourceMap* map = &state.context.source_map;
    if (file != state.context.file) {
        if (state.context.types != nullptr) {
            if (const jsx::SourceMap* foreign = state.context.types->map_of(file)) {
                map = foreign;
            }
        }
        message += " (in `" + std::string(file) + "`)";
    }
    state.error = jsx::Diagnostic{code,
                                  jsx::Severity::Error,
                                  std::move(message),
                                  map->resolve(span),
                                  std::string(suggestion),
                                  std::nullopt};
}

enum class MembersKind : std::uint8_t { None, Found, Error };

// The declared members of the props type, or `None` when the component declares no props parameter
// or its type is not an object. With a `TypeNameTable` the name is resolved across project files,
// following imports, `as` renames and re-export chains to the one declaration that owns the object
// body (issue #70); a name declared as a component/constant, a missing name inside an imported
// alias and a cycle are loud. Without a table only a same-file alias chain is followed, and a name
// with no declaration keeps the pre-slice-4 behavior (the documented gap for entry points that
// lower a single component).
MembersKind props_members(NodeState& state, MembersView& view) {
    const jsx::ModuleItem& component = state.context.component;
    if (!component.has_props || !component.props_type) {
        return MembersKind::None;
    }
    const jsx::TypeNode* type = component.props_type.get();
    std::string_view scope = state.context.file;
    std::unordered_set<const jsx::ModuleItem*> visited;
    for (std::size_t depth = 0; depth < kMaxAliasDepth; ++depth) {
        if (type->kind == jsx::TypeKind::Object) {
            view = MembersView{&type->members, scope};
            return MembersKind::Found;
        }
        if (type->kind != jsx::TypeKind::Name || !type->children.empty()) {
            return MembersKind::None; // a union/function/generic props type has no prop records
        }
        if (state.context.types != nullptr) {
            const TypeNameEntry* entry = state.context.types->find(scope, type->text);
            if (entry == nullptr) {
                if (scope == state.context.file) {
                    return MembersKind::None;
                }
                fail_type(state, "TESSERA_IR_TYPE_UNRESOLVED",
                          "type `" + type->text + "` is not declared in `" + std::string(scope) +
                              "`",
                          scope, type->span);
                return MembersKind::Error;
            }
            if (entry->kind == TypeNameKind::Value) {
                fail_type(state, "TESSERA_IR_TYPE_NOT_ALIAS",
                          "`" + type->text + "` is declared in `" + std::string(scope) +
                              "` as a component or constant, not as a type alias",
                          scope, type->span, "declare the props type with `type`");
                return MembersKind::Error;
            }
            if (entry->alias == nullptr || !entry->alias->type) {
                fail_type(state, "TESSERA_IR_MALFORMED",
                          "type alias `" + type->text + "` has no target type", entry->alias_file,
                          type->span);
                return MembersKind::Error;
            }
            if (!visited.insert(entry->alias).second) {
                fail_type(state, "TESSERA_IR_TYPE_CYCLE",
                          "type-alias cycle through `" + type->text + "`", entry->alias_file,
                          type->span, "break the alias chain");
                return MembersKind::Error;
            }
            type = entry->alias->type.get();
            scope = entry->alias_file;
            continue;
        }
        if (state.context.module == nullptr) {
            return MembersKind::None;
        }
        const jsx::ModuleItem* alias = find_alias(*state.context.module, type->text);
        if (alias == nullptr || !alias->type) {
            return MembersKind::None;
        }
        if (!visited.insert(alias).second) {
            fail_type(state, "TESSERA_IR_TYPE_CYCLE",
                      "type-alias cycle through `" + type->text + "`", scope, type->span,
                      "break the alias chain");
            return MembersKind::Error;
        }
        type = alias->type.get();
    }
    fail_type(state, "TESSERA_IR_UNSUPPORTED",
              "the props type alias chain is deeper than " + std::to_string(kMaxAliasDepth) +
                  " aliases",
              scope, type->span);
    return MembersKind::Error;
}

} // namespace

DeclaredPropsKind lower_declared_props(NodeState& state, std::vector<ir::Prop>& props) {
    MembersView view;
    const MembersKind kind = props_members(state, view);
    if (kind == MembersKind::None) {
        return DeclaredPropsKind::None;
    }
    if (kind == MembersKind::Error) {
        return DeclaredPropsKind::Error;
    }
    const std::vector<jsx::TypeMember>& members = *view.members;
    std::unordered_set<std::string> names;
    for (const jsx::TypeMember& member : members) {
        if (member.name.empty() || !member.type) {
            fail_type(state, "TESSERA_IR_MALFORMED",
                      "a declared prop record needs a name and a type", view.file, member.span);
            return DeclaredPropsKind::Error;
        }
        if (!names.insert(member.name).second) {
            fail_type(state, "TESSERA_IR_DUPLICATE_ID", "duplicate prop `" + member.name + "`",
                      view.file, member.span);
            return DeclaredPropsKind::Error;
        }
        const std::optional<std::string> type = lower_prop_type(*member.type);
        if (!type) {
            fail_type(state, "TESSERA_IR_UNSUPPORTED",
                      "prop `" + member.name + "` has a type with no IR form in v1 (`" +
                          jsx::dump(*member.type) + "`)",
                      view.file, member.type->span,
                      "write an IR type, for example `string`, `int`, `T[]`, `optional<...>` or "
                      "a payload record name");
            return DeclaredPropsKind::Error;
        }
        if (member.name == "children") {
            // Section 3.4: the slot has the `children` type, no default and no `?`.
            if (member.optional || *type != "children") {
                fail_type(state, "TESSERA_IR_SLOT",
                          "the `children` prop declares the static slot, so it has type "
                          "`Children` and no `?`",
                          view.file, member.span);
                return DeclaredPropsKind::Error;
            }
            if (const jsx::PropBinding* binding = find_binding(state, member.name);
                binding != nullptr && binding->default_value) {
                fail_node(state, "TESSERA_IR_SLOT", "the `children` prop has no default value",
                          binding->span);
                return DeclaredPropsKind::Error;
            }
            props.push_back(ir::Prop{"children", "children", nullptr});
            continue;
        }
        if (*type == "children") {
            fail_type(state, "TESSERA_IR_SLOT",
                      "prop `" + member.name + "` cannot have the `children` slot type", view.file,
                      member.span);
            return DeclaredPropsKind::Error;
        }
        // The destructuring on the props parameter is where a default is authored; the alias only
        // carries `?`. Lowering keeps both records: `optional` is the "may be omitted" flag and
        // `default_value` is the value codegen initializes the prop field with.
        ir::ExprPtr default_value;
        if (const jsx::PropBinding* binding = find_binding(state, member.name);
            binding != nullptr && binding->default_value) {
            if (!is_literal_default(*binding->default_value)) {
                fail_node(state, "TESSERA_IR_UNSUPPORTED",
                          "default of prop `" + member.name +
                              "` must be a literal or a literal list",
                          binding->span);
                return DeclaredPropsKind::Error;
            }
            default_value = lower_node_expression(state, *binding->default_value);
            if (!default_value) {
                return DeclaredPropsKind::Error;
            }
        }
        props.push_back(ir::Prop{member.name, *type, std::move(default_value), member.optional});
    }
    // A default on a name the alias does not declare would silently disappear; reject it instead.
    for (const jsx::PropBinding& binding : state.context.component.props_bindings) {
        if (binding.default_value && names.find(binding.name) == names.end()) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED",
                      "default of `" + binding.name + "` has no declared prop record",
                      binding.span);
            return DeclaredPropsKind::Error;
        }
    }
    return DeclaredPropsKind::Lowered;
}

} // namespace tessera::compiler::lowering::internal
