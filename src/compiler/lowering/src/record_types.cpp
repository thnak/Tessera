// SPDX-License-Identifier: MIT

// Slice 3 of ADR-051: the module-level record type table (docs/specs/ir-v1.md section 2.2). Every
// `type X = {...}` of the project becomes one `ir::RecordType`, whether or not a component uses it
// (the IR validator warns about an unused one). An alias of an alias resolves to the one object
// body it ultimately names, so where the body is authored cannot change the table; an alias of a
// primitive, union, array or function type is not a record and is skipped.

#include "hook_lowering_internal.hpp"
#include "record_internal.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/source_span.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tessera::compiler::lowering::internal {
namespace {

constexpr std::size_t kMaxAliasDepth = 8;

// `name` in `file` resolved to the object body it ultimately declares, or null when the chain ends
// at a non-object type (a primitive alias is not a record), names nothing, or cycles. A cycle is
// not reported here: the props path reports one at the span the developer wrote, and an alias cycle
// no component reaches is not a record at all.
const jsx::TypeNode* resolve_object_body(const TypeNameTable& names, std::string_view file,
                                         std::string_view name) {
    std::unordered_set<std::string> visited;
    std::string_view scope = file;
    std::string_view current = name;
    for (std::size_t depth = 0; depth < kMaxAliasDepth; ++depth) {
        std::string key(scope);
        key += '\n';
        key += current;
        if (!visited.insert(std::move(key)).second) {
            return nullptr;
        }
        const TypeNameEntry* entry = names.find(scope, current);
        if (entry == nullptr || entry->kind != TypeNameKind::Alias || entry->alias == nullptr ||
            !entry->alias->type) {
            return nullptr;
        }
        const jsx::TypeNode& type = *entry->alias->type;
        if (type.kind == jsx::TypeKind::Object) {
            return &type;
        }
        if (type.kind != jsx::TypeKind::Name || !type.children.empty() || type.text.empty()) {
            return nullptr;
        }
        scope = entry->alias_file;
        current = type.text;
    }
    return nullptr;
}

jsx::Diagnostic malformed(std::string message, const jsx::SourceMap& map, jsx::Span span) {
    return jsx::Diagnostic{"TESSERA_IR_MALFORMED",
                           jsx::Severity::Error,
                           std::move(message),
                           map.resolve(span),
                           {},
                           std::nullopt,
                           {}};
}

// The aliases that spell one component's props type, followed through the same import/alias chain
// the props lowering uses (`props_members`). Those aliases are the component *signature*: they
// become `Component::props`, may hold `callback`/`children` members (ADR-051 section 1 forbids both
// in a record), and are not value records. Listing them in `types` would also give every module
// with a props alias a `types` entry, which is exactly the "existing goldens do not change" the ADR
// promises against.
void collect_props_aliases(const jsx::Module& module, std::string_view file,
                           const TypeNameTable& names,
                           std::unordered_set<const jsx::ModuleItem*>& out) {
    for (const jsx::ModuleItem& item : module.items) {
        if (item.kind != jsx::ItemKind::Component || !item.props_type) {
            continue;
        }
        const jsx::TypeNode* type = item.props_type.get();
        std::string_view scope = file;
        for (std::size_t depth = 0; type != nullptr && depth < kMaxAliasDepth; ++depth) {
            if (type->kind != jsx::TypeKind::Name || !type->children.empty()) {
                break;
            }
            const TypeNameEntry* entry = names.find(scope, type->text);
            if (entry == nullptr || entry->kind != TypeNameKind::Alias || entry->alias == nullptr ||
                !entry->alias->type) {
                break;
            }
            if (!out.insert(entry->alias).second) {
                break;
            }
            type = entry->alias->type.get();
            scope = entry->alias_file;
        }
    }
}

// --- Anonymous record types (issue #804) ---------------------------------------------------------
//
// A prop written `{ rows: { id: string }[] }` has no `type X = ...` alias, so `record_name_of` has
// nothing to resolve against and the prop is `TESSERA_IR_UNSUPPORTED`. The owner's decision for
// #804 is to keep ADR-051's name-identity model and synthesize a deterministic internal name for
// every anonymous object type, collect it into `types`, and let the prop type name it.
//
// The name is derived from the file stem and the object's declaration order (`Rows`, `Rows_2` on a
// collision), so the same source always produces the same table; it is not a documented ported
// symbol, which is why the codegen note marks the generated struct as derived from an anonymous
// type. A `type X = ...` alias for the same shape keeps its declared name and wins over a
// synthesized one — the anonymous collection only sees type positions no alias names.

// The PascalCase base of a synthesized record name: the file's stem (`filter-map.tsx` ->
// `FilterMap`). Both separators are handled, because the project-relative path a diagnostic names
// is POSIX but a caller-built path on Windows may carry `\`.
[[nodiscard]] std::string stem_base(std::string_view file) {
    const std::size_t slash = file.find_last_of("/\\");
    std::string_view stem = slash == std::string_view::npos ? file : file.substr(slash + 1);
    const std::size_t dot = stem.rfind('.');
    if (dot != std::string_view::npos) {
        stem = stem.substr(0, dot);
    }
    std::string out;
    out.reserve(stem.size());
    bool upper = true;
    for (const char c : stem) {
        if (c == '-' || c == '_' || c == ' ') {
            upper = true;
            continue;
        }
        out.push_back(upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
        upper = false;
    }
    if (out.empty() || out.front() < 'A' || out.front() > 'Z') {
        return "Anon";
    }
    return out;
}

// True when `name` is already a declared record, a component, a handler, or otherwise taken in this
// module. Only the declared-record check is cheap here; the caller's `used` set covers the
// synthesized names issued so far, and a collision with a *declared* name is resolved by suffixing,
// because a synthesized name must never shadow a real one.
[[nodiscard]] bool name_taken(std::string_view name, const TypeNameTable& names,
                              std::string_view file, const std::unordered_set<std::string>& used) {
    if (used.contains(std::string(name))) {
        return true;
    }
    // A declared alias would make `record_name_of` ambiguous between the two spellings of the same
    // shape, so a synthesized name skips it too. Components/constants are not in the record table
    // and cannot collide in C++ (different namespaces), but the alias check is the one that
    // matters.
    return names.find(file, name) != nullptr;
}

// The IR types of one anonymous object's members, collected into `record`. A member with no IR form
// or a `children`/callback member keeps the anonymous type unsupported — the prop that names it
// reports `TESSERA_IR_UNSUPPORTED` at its own span, exactly as before, rather than the collection
// failing the whole module.
[[nodiscard]] bool anonymous_members(const jsx::TypeNode& body, ir::RecordType& record) {
    for (const jsx::TypeMember& member : body.members) {
        if (member.name.empty() || !member.type) {
            return false;
        }
        // Nested objects inside an anonymous member are not followed here: a one-level anonymous
        // shape is what the corpus measures, and a member of record type needs a declared name to
        // resolve against (the fix for a nested anonymous member is the same synthesis applied
        // recursively, which slice keeps out of scope rather than half-doing).
        const std::optional<std::string> type = lower_written_value_type(*member.type);
        if (!type || *type == "children" || *type == "callback") {
            return false;
        }
        record.members.push_back(ir::RecordMember{member.name, *type, member.optional});
    }
    return true;
}

// The anonymous object types of one component's props type: every `Object` node found by walking
// the resolved props type's structure (the members, and the element of an array). Returns the
// synthesized records and fills `node_names` with `node -> name` for the prop lowering to consult.
void collect_anonymous_props(const jsx::Module& module, std::string_view file,
                             const jsx::SourceMap& source_map, const TypeNameTable& names,
                             std::vector<ir::RecordType>& out,
                             std::unordered_map<const jsx::TypeNode*, std::string>& node_names,
                             std::unordered_set<std::string>& used) {
    const std::string base = stem_base(file);
    for (const jsx::ModuleItem& item : module.items) {
        if (item.kind != jsx::ItemKind::Component || !item.props_type) {
            continue;
        }
        // The props type itself is the component *signature* — `props_members` handles it directly
        // and ADR-051 forbids listing a signature in `types` (see `collect_props_aliases`). What
        // needs synthesizing is an object type in a member's *value* position, which is the
        // element of an array prop (`rows: { id: string }[]`).
        for (const jsx::TypeMember& member : item.props_type->members) {
            if (!member.type) {
                continue;
            }
            const jsx::TypeNode* object = member.type.get();
            if (object->kind == jsx::TypeKind::Array && object->children.size() == 1 &&
                object->children[0]) {
                object = object->children[0].get();
            }
            if (object->kind != jsx::TypeKind::Object || node_names.contains(object)) {
                continue;
            }
            ir::RecordType record;
            std::string name = base;
            for (int suffix = 2; name_taken(name, names, file, used); ++suffix) {
                name = base + "_" + std::to_string(suffix);
            }
            used.insert(name);
            record.id = "type::" + name;
            record.name = name;
            record.source = to_source_span(source_map, object->span, file);
            if (!anonymous_members(*object, record)) {
                // Leave the type unregistered: the prop reports its own
                // `TESSERA_IR_UNSUPPORTED` with the spelling the developer wrote.
                continue;
            }
            node_names.emplace(object, std::move(name));
            out.push_back(std::move(record));
        }
    }
}

} // namespace

std::expected<CollectedRecords, jsx::Diagnostic>
collect_record_types(std::span<const RecordTypeSource> files, const TypeNameTable& names) {
    std::vector<ir::RecordType> table;
    std::unordered_set<std::string> declared;
    std::unordered_set<const jsx::ModuleItem*> props_aliases;
    for (const RecordTypeSource& file : files) {
        if (file.module != nullptr) {
            collect_props_aliases(*file.module, file.file, names, props_aliases);
        }
    }
    // Anonymous record types (issue #804), collected in the same pass so one table carries both
    // the declared records and the synthesized ones, still sorted by name at the end.
    std::unordered_map<const jsx::TypeNode*, std::string> anonymous_nodes;
    std::unordered_set<std::string> synthesized;
    for (const RecordTypeSource& file : files) {
        if (file.module == nullptr || file.source_map == nullptr) {
            continue;
        }
        collect_anonymous_props(*file.module, file.file, *file.source_map, names, table,
                                anonymous_nodes, synthesized);
    }
    for (const RecordTypeSource& file : files) {
        if (file.module == nullptr || file.source_map == nullptr) {
            continue;
        }
        for (const jsx::ModuleItem& item : file.module->items) {
            if (item.kind != jsx::ItemKind::TypeAlias || item.name.empty()) {
                continue;
            }
            if (props_aliases.contains(&item)) {
                continue; // a component signature, not a value record
            }
            const jsx::TypeNode* body = resolve_object_body(names, file.file, item.name);
            if (body == nullptr) {
                continue; // not a record: a primitive/union/array/function alias, or a cycle
            }
            // ADR-051 section 1: the identity is the name, as for components. Two files declaring
            // the same record name is a duplicate id, never a merge.
            if (!declared.insert(item.name).second) {
                return std::unexpected(
                    malformed("record type `" + item.name + "` is declared in two files",
                              *file.source_map, item.span));
            }
            ir::RecordType record;
            record.id = "type::" + item.name;
            record.name = item.name;
            record.source = to_source_span(*file.source_map, item.span, file.file);
            for (const jsx::TypeMember& member : body->members) {
                if (member.name.empty() || !member.type) {
                    return std::unexpected(
                        malformed("a member of record `" + item.name + "` needs a name and a type",
                                  *file.source_map, member.span));
                }
                const std::optional<std::string> type = lower_written_value_type(*member.type);
                if (!type) {
                    return std::unexpected(malformed("member `" + member.name + "` of record `" +
                                                         item.name +
                                                         "` has a type with no IR form in v1 (`" +
                                                         jsx::dump(*member.type) + "`)",
                                                     *file.source_map, member.type->span));
                }
                // ADR-051 section 1: a record member is a value, so the `children` slot and a
                // callback have no record form. The IR validator enforces this too, at the record's
                // own span; rejecting here keeps the diagnostic on the member the developer wrote.
                if (*type == "children" || *type == "callback") {
                    return std::unexpected(malformed("member `" + member.name + "` of record `" +
                                                         item.name + "` is `" + *type +
                                                         "`; a record holds values only",
                                                     *file.source_map, member.type->span));
                }
                record.members.push_back(ir::RecordMember{member.name, *type, member.optional});
            }
            table.push_back(std::move(record));
        }
    }
    // ir-v1 section 11: record types sorted by name. An anonymous record's node pointer is only
    // valid while the parsed ASTs live, so the registry is folded into the return value as
    // (record table, name) pairs rather than kept as pointers into `out`.
    std::sort(table.begin(), table.end(),
              [](const ir::RecordType& left, const ir::RecordType& right) {
                  return left.name < right.name;
              });
    return CollectedRecords{std::move(table), std::move(anonymous_nodes)};
}

std::optional<std::string_view> list_element_of(std::string_view type) {
    constexpr std::string_view kList = "list<";
    if (type.size() <= kList.size() + 1 || type.rfind(kList, 0) != 0 || type.back() != '>') {
        return std::nullopt;
    }
    return type.substr(kList.size(), type.size() - kList.size() - 1);
}

std::optional<std::string_view> record_name_of(std::span<const ir::RecordType> table,
                                               std::string_view type) {
    constexpr std::string_view kOptional = "optional<";
    std::string_view name = type;
    while (name.size() > kOptional.size() + 1 && name.rfind(kOptional, 0) == 0 &&
           name.back() == '>') {
        name = name.substr(kOptional.size(), name.size() - kOptional.size() - 1);
    }
    if (find_record(table, name) == nullptr) {
        return std::nullopt;
    }
    return name;
}

const ir::RecordType* find_record(std::span<const ir::RecordType> table, std::string_view name) {
    for (const ir::RecordType& record : table) {
        if (record.name == name) {
            return &record;
        }
    }
    return nullptr;
}

} // namespace tessera::compiler::lowering::internal
