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
                           std::nullopt};
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

} // namespace

std::expected<std::vector<ir::RecordType>, jsx::Diagnostic>
collect_record_types(std::span<const RecordTypeSource> files, const TypeNameTable& names) {
    std::vector<ir::RecordType> table;
    std::unordered_set<std::string> declared;
    std::unordered_set<const jsx::ModuleItem*> props_aliases;
    for (const RecordTypeSource& file : files) {
        if (file.module != nullptr) {
            collect_props_aliases(*file.module, file.file, names, props_aliases);
        }
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
    // ir-v1 section 11: record types sorted by name.
    std::sort(table.begin(), table.end(),
              [](const ir::RecordType& left, const ir::RecordType& right) {
                  return left.name < right.name;
              });
    return table;
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
