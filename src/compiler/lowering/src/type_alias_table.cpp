// SPDX-License-Identifier: MIT

// Slice 4 (cross-file): resolves the type aliases behind a component's props parameter across
// project files (issue #70). A props type may name a local alias, an alias imported with
// `import type`/`import`, an `as` rename, or a name re-exported by another file (including a
// chain of re-exports); every path ends at the one `type` declaration that owns the object body, so
// the props records do not depend on where the alias is authored
// (docs/design/jsx-ir-lowering.md "Types (props records/defaults done)").

#include "type_scope.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::lowering {

const TypeNameEntry* TypeNameTable::find(std::string_view file, std::string_view name) const {
    const auto found = std::lower_bound(
        entries.begin(), entries.end(), std::pair<std::string_view, std::string_view>{file, name},
        [](const TypeNameEntry& entry, const std::pair<std::string_view, std::string_view>& key) {
            if (entry.file != key.first) {
                return entry.file < key.first;
            }
            return entry.name < key.second;
        });
    if (found == entries.end() || found->file != file || found->name != name) {
        return nullptr;
    }
    return &*found;
}

const jsx::SourceMap* TypeNameTable::map_of(std::string_view file) const {
    const auto found = std::lower_bound(
        files.begin(), files.end(), file,
        [](const TypeFileMap& entry, std::string_view key) { return entry.file < key; });
    if (found == files.end() || found->file != file) {
        return nullptr;
    }
    return found->map;
}

} // namespace tessera::compiler::lowering

namespace tessera::compiler::lowering::internal {
namespace {

struct Resolution {
    TypeNameKind kind = TypeNameKind::Alias;
    const jsx::ModuleItem* alias = nullptr; // null: the name is not declared
    std::size_t file = kNoProjectFile;
};

// One resolution path. Each recursive step follows exactly one import or re-export link, so a
// revisit of the same (file, name) pair is a cycle and not a diamond.
struct Chain {
    std::vector<std::pair<std::size_t, std::string>> visited;
    bool cycle = false;
};

const jsx::ModuleItem* find_local(const jsx::Module& module, std::string_view name,
                                  TypeNameKind& kind) {
    for (const jsx::ModuleItem& item : module.items) {
        if (item.name != name) {
            continue;
        }
        if (item.kind == jsx::ItemKind::TypeAlias) {
            kind = TypeNameKind::Alias;
            return &item;
        }
        if (item.kind == jsx::ItemKind::Component || item.kind == jsx::ItemKind::Const) {
            kind = TypeNameKind::Value;
            return &item;
        }
    }
    return nullptr;
}

// The declaration of `name` exported by `index`, following imports and re-exports.
Resolution resolve_name(const std::span<const ProjectFileScope>& files, std::size_t index,
                        std::string_view name, Chain& chain) {
    for (const auto& [file, visited_name] : chain.visited) {
        if (file == index && visited_name == name) {
            chain.cycle = true;
            return {};
        }
    }
    chain.visited.emplace_back(index, std::string(name));
    TypeNameKind kind = TypeNameKind::Alias;
    if (const jsx::ModuleItem* local = find_local(*files[index].module, name, kind)) {
        return Resolution{kind, local, index};
    }
    for (const ProjectModuleRef& reference : files[index].imports) {
        for (const jsx::ImportName& imported : reference.item->names) {
            if (imported.local() != name) {
                continue;
            }
            const Resolution inner = resolve_name(files, reference.target, imported.name, chain);
            if (chain.cycle) {
                return {};
            }
            return inner;
        }
    }
    for (const ProjectModuleRef& reference : files[index].re_exports) {
        for (const jsx::ImportName& exported : reference.item->names) {
            if (exported.local() != name) {
                continue;
            }
            const std::size_t target =
                reference.target == kNoProjectFile ? index : reference.target;
            return resolve_name(files, target, exported.name, chain);
        }
    }
    return {};
}

} // namespace

ProjectTypeNames build_type_names(std::span<const ProjectFileScope> files) {
    ProjectTypeNames out;
    for (const ProjectFileScope& file : files) {
        out.files.push_back(TypeFileMap{file.file, file.source_map});
    }
    std::sort(
        out.files.begin(), out.files.end(),
        [](const TypeFileMap& left, const TypeFileMap& right) { return left.file < right.file; });
    const auto fail = [&out, files](std::size_t index, std::string_view code, std::string message,
                                    jsx::Span span) {
        if (out.error || files[index].source_map == nullptr) {
            return;
        }
        out.error = jsx::Diagnostic{
            code, jsx::Severity::Error, std::move(message), files[index].source_map->resolve(span),
            {},   std::nullopt};
    };
    // Local declarations first: a declaration shadows an imported name of the same spelling, and
    // the stable sort keeps that order.
    for (const ProjectFileScope& file : files) {
        for (const jsx::ModuleItem& item : file.module->items) {
            if (item.name.empty()) {
                continue;
            }
            if (item.kind == jsx::ItemKind::TypeAlias) {
                out.entries.push_back(
                    TypeNameEntry{file.file, item.name, TypeNameKind::Alias, file.file, &item});
            } else if (item.kind == jsx::ItemKind::Const || item.kind == jsx::ItemKind::Component) {
                out.entries.push_back(
                    TypeNameEntry{file.file, item.name, TypeNameKind::Value, {}, nullptr});
            }
        }
    }
    for (std::size_t index = 0; index < files.size(); ++index) {
        const ProjectFileScope& file = files[index];
        for (const ProjectModuleRef& reference : file.imports) {
            for (const jsx::ImportName& imported : reference.item->names) {
                Chain chain;
                const Resolution resolved =
                    resolve_name(files, reference.target, imported.name, chain);
                if (chain.cycle) {
                    fail(index, "TESSERA_IR_TYPE_CYCLE",
                         "type-alias import cycle while resolving `" + imported.name + "` from `" +
                             reference.item->source + "`",
                         imported.span);
                    return out;
                }
                if (resolved.alias == nullptr) {
                    const std::string_view code = reference.item->type_only
                                                      ? "TESSERA_IR_TYPE_UNRESOLVED"
                                                      : "TESSERA_IR_UNRESOLVED";
                    fail(index, code,
                         "`" + imported.name + "` is not exported by `" + reference.item->source +
                             "`",
                         imported.span);
                    return out;
                }
                out.entries.push_back(TypeNameEntry{
                    file.file, imported.local(), resolved.kind, files[resolved.file].file,
                    resolved.kind == TypeNameKind::Alias ? resolved.alias : nullptr});
            }
        }
    }
    std::stable_sort(out.entries.begin(), out.entries.end(),
                     [](const TypeNameEntry& left, const TypeNameEntry& right) {
                         if (left.file != right.file) {
                             return left.file < right.file;
                         }
                         return left.name < right.name;
                     });
    out.entries.erase(std::unique(out.entries.begin(), out.entries.end(),
                                  [](const TypeNameEntry& left, const TypeNameEntry& right) {
                                      return left.file == right.file && left.name == right.name;
                                  }),
                      out.entries.end());
    out.table.entries = std::span<const TypeNameEntry>(out.entries);
    out.table.files = std::span<const TypeFileMap>(out.files);
    return out;
}

} // namespace tessera::compiler::lowering::internal
