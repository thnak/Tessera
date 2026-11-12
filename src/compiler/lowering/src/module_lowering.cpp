// SPDX-License-Identifier: MIT

// Slice 9: module- and project-level lowering (docs/design/jsx-ir-lowering.md section 2 and
// decision 13). Every component declaration of every file is lowered once with the slice 6 to 8
// helpers; a component tag resolves to a component declared in the same file or imported from
// another project file, and becomes a `Component` node naming it (never an inlined subtree). File
// reading stays with the caller.

#include "node_lowering_internal.hpp"
#include "tessera/compiler/lowering/library_components.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::lowering {
namespace {

// One input file with its cross-file component references resolved.
struct FileEntry {
    const ModuleSource* source = nullptr;
    std::vector<ComponentRef> component_refs; // local declarations and imported components
    std::vector<std::size_t> import_targets;  // indices of files this file imports from
    std::vector<std::string> declared_names;  // component names declared here
};

struct ProjectState {
    std::vector<FileEntry> files;
    std::vector<std::size_t> order;             // file indices sorted by path, the lowering order
    std::vector<ir::StylesheetRef> stylesheets; // deduplicated, sorted at the end
    std::optional<jsx::Diagnostic> error;
};

// FNV-1a 64 over the file bytes: deterministic, dependency-free, and enough to detect a changed
// stylesheet (it is a change marker, not a security digest).
std::string stylesheet_hash(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : text) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    }
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string out = "fnv1a64:";
    for (int shift = 60; shift >= 0; shift -= 4) {
        out.push_back(kHex[(hash >> shift) & 0xF]);
    }
    return out;
}

jsx::Diagnostic make_diagnostic(const ModuleSource& source, std::string_view code,
                                std::string_view message, jsx::Span span,
                                std::string_view suggestion = {}) {
    return jsx::Diagnostic{code,
                           jsx::Severity::Error,
                           std::string(message),
                           source.source_map != nullptr ? source.source_map->resolve(span)
                                                        : jsx::ResolvedSpan{},
                           std::string(suggestion),
                           std::nullopt};
}

void fail(ProjectState& state, const ModuleSource& source, std::string_view code,
          std::string_view message, jsx::Span span, std::string_view suggestion = {}) {
    if (!state.error) {
        state.error = make_diagnostic(source, code, message, span, suggestion);
    }
}

std::string directory_of(std::string_view file) {
    const std::size_t slash = file.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(file.substr(0, slash));
}

// "ui/Counter.tsx" -> "ui/Counter"; an import source omits the extension (subset section 4).
std::string without_extension(std::string_view path) {
    const std::size_t dot = path.rfind('.');
    const std::size_t slash = path.rfind('/');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
        return std::string(path);
    }
    return std::string(path.substr(0, dot));
}

const jsx::ModuleItem* find_declaration(const jsx::Module& module, std::string_view name) {
    for (const jsx::ModuleItem& item : module.items) {
        if ((item.kind == jsx::ItemKind::Component || item.kind == jsx::ItemKind::TypeAlias ||
             item.kind == jsx::ItemKind::Const) &&
            item.name == name) {
            return &item;
        }
    }
    return nullptr;
}

bool has_import_cycle(const std::vector<FileEntry>& files, std::size_t start,
                      std::vector<int>& color) {
    if (color[start] == 1) {
        return true;
    }
    if (color[start] == 2) {
        return false;
    }
    color[start] = 1;
    for (const std::size_t target : files[start].import_targets) {
        if (has_import_cycle(files, target, color)) {
            return true;
        }
    }
    color[start] = 2;
    return false;
}

bool has_ref(const FileEntry& file, std::string_view name) {
    for (const ComponentRef& reference : file.component_refs) {
        if (reference.name == name) {
            return true;
        }
    }
    return false;
}

constexpr std::size_t kNoFile = static_cast<std::size_t>(-1);

// Index of the project file whose path (without its extension) is `key`, or kNoFile.
std::size_t find_project_file(const ProjectState& state, std::string_view key) {
    for (std::size_t candidate = 0; candidate < state.files.size(); ++candidate) {
        if (without_extension(state.files[candidate].source->file) == key) {
            return candidate;
        }
    }
    return kNoFile;
}

// Resolves a side-effect CSS import against the caller-provided files and records it once.
bool resolve_stylesheet(ProjectState& state, const ModuleSource& source,
                        const jsx::ModuleItem& item, std::span<const StylesheetFile> stylesheets) {
    const std::string path = join_project_path(directory_of(source.file), item.source);
    const auto found =
        std::find_if(stylesheets.begin(), stylesheets.end(),
                     [&](const StylesheetFile& sheet) { return sheet.path == path; });
    if (found == stylesheets.end()) {
        fail(state, source, "TESSERA_IR_STYLESHEET_MISSING",
             "stylesheet `" + item.source + "` (`" + path + "`) is missing or unreadable",
             item.span, "create the file or fix the import path");
        return false;
    }
    const bool known =
        std::any_of(state.stylesheets.begin(), state.stylesheets.end(),
                    [&](const ir::StylesheetRef& sheet) { return sheet.path == path; });
    if (!known) {
        state.stylesheets.push_back(ir::StylesheetRef{"css::" + without_extension(path), path,
                                                      stylesheet_hash(found->text)});
    }
    return true;
}

// Builds the component references and import graph of one file. False after reporting an error.
bool resolve_imports(ProjectState& state, std::size_t index, std::span<const PackageRoot> packages,
                     std::span<const StylesheetFile> stylesheets) {
    FileEntry& entry = state.files[index];
    const ModuleSource& source = *entry.source;
    for (const jsx::ModuleItem& item : source.module->items) {
        if (item.kind == jsx::ItemKind::Component) {
            if (std::find(entry.declared_names.begin(), entry.declared_names.end(), item.name) !=
                entry.declared_names.end()) {
                fail(state, source, "TESSERA_IR_MALFORMED",
                     "component `" + item.name + "` is declared twice in this file", item.span);
                return false;
            }
            entry.declared_names.push_back(item.name);
            entry.component_refs.push_back(ComponentRef{item.name, "comp::" + item.name});
        }
    }
    for (const jsx::ModuleItem& item : source.module->items) {
        if (item.kind == jsx::ItemKind::Import && item.side_effect) {
            if (!resolve_stylesheet(state, source, item, stylesheets)) {
                return false;
            }
            continue;
        }
        if (item.kind != jsx::ItemKind::Import) {
            continue;
        }
        const bool relative = item.source.rfind("./", 0) == 0 || item.source.rfind("../", 0) == 0;
        std::string target_key;
        if (!relative) {
            if (item.source == "tessera") {
                // A framework import may name a library component
                // (docs/design/component-library.md): it becomes an ordinary component reference,
                // and the module gets the component's IR declaration. Hooks, intrinsics and types
                // keep resolving through the schema.
                for (const jsx::ImportName& name : item.names) {
                    const LibraryComponent* library = find_library_component(name.name);
                    if (library != nullptr && !has_ref(entry, name.name)) {
                        entry.component_refs.push_back(
                            ComponentRef{name.name, std::string(library->id)});
                    }
                }
                continue;
            }
            if (item.source == "react") {
                continue; // React-compatible names resolve through the intrinsic schema table
            }
            // A bare specifier names a configured package root (ADR-014): `pkg/Sub` resolves to
            // `<root>/Sub` and a bare `pkg` to `<root>/index`. Everything after that follows the
            // ordinary cross-file rules, so a package is compiled by the same pipeline.
            const PackageRoot* package = nullptr;
            std::string_view sub;
            for (const PackageRoot& candidate : packages) {
                if (item.source == candidate.name) {
                    package = &candidate;
                    sub = {};
                    break;
                }
                if (item.source.size() > candidate.name.size() &&
                    item.source.compare(0, candidate.name.size(), candidate.name) == 0 &&
                    item.source[candidate.name.size()] == '/') {
                    package = &candidate;
                    sub = std::string_view(item.source).substr(candidate.name.size() + 1);
                    break;
                }
            }
            if (package == nullptr) {
                fail(state, source, "TESSERA_IR_UNSUPPORTED",
                     "import `" + item.source +
                         "` is neither a project file (`./X`) nor a configured package",
                     item.span,
                     "use a relative import or declare the package in the project config");
                return false;
            }
            target_key = sub.empty() ? join_project_path(package->root, "index")
                                     : join_project_path(package->root, sub);
        } else {
            target_key = join_project_path(directory_of(source.file), item.source);
        }
        const std::size_t target = find_project_file(state, target_key);
        if (target == kNoFile) {
            fail(state, source, "TESSERA_IR_UNRESOLVED",
                 "import `" + item.source + "` does not name a file of this project", item.span);
            return false;
        }
        entry.import_targets.push_back(target);
        const jsx::Module& target_module = *state.files[target].source->module;
        for (const jsx::ImportName& name : item.names) {
            if (has_ref(entry, name.name)) {
                continue; // the same name imported twice; the first wins deterministically
            }
            const jsx::ModuleItem* declaration = find_declaration(target_module, name.name);
            if (declaration == nullptr) {
                fail(state, source, "TESSERA_IR_UNRESOLVED",
                     "`" + name.name + "` is not exported by `" + item.source + "`", name.span);
                return false;
            }
            if (declaration->kind == jsx::ItemKind::Component) {
                entry.component_refs.push_back(ComponentRef{name.name, "comp::" + name.name});
            }
            // Type aliases and constants are imported for later slices; they have no IR yet.
        }
    }
    return true;
}

std::optional<LoweredModule> finish(ProjectState& state, std::string_view module_name) {
    LoweredModule lower;
    lower.name = std::string(module_name);
    for (const std::size_t index : state.order) {
        const FileEntry& entry = state.files[index];
        const ModuleSource& source = *entry.source;
        for (std::size_t item_index = 0; item_index < source.module->items.size(); ++item_index) {
            const jsx::ModuleItem& item = source.module->items[item_index];
            if (item.kind != jsx::ItemKind::Component) {
                continue;
            }
            const rules::ComponentAnalysis* analysis = source.analysis->find_component(item_index);
            if (analysis == nullptr) {
                fail(state, source, "TESSERA_IR_MALFORMED",
                     "component `" + item.name + "` was not analysed", item.span);
                return std::nullopt;
            }
            const HookLoweringContext context{
                item,         *analysis, *source.source_map, source.file, entry.component_refs,
                source.module};
            LoweredComponentResult component = lower_component(context);
            if (!component) {
                if (!state.error && !component.error().empty()) {
                    state.error = component.error().front();
                }
                return std::nullopt;
            }
            for (ir::Binding& binding : component->bindings) {
                lower.bindings.push_back(std::move(binding));
            }
            lower.components.push_back(std::move(component->component));
        }
    }
    // Library components are IR data, not project files: include each referenced one once. A
    // project component of the same name wins (the local declaration is the reference target), so
    // the built-in declaration is only added when the name is not declared anywhere.
    std::vector<std::string> referenced_library;
    for (const FileEntry& entry : state.files) {
        for (const ComponentRef& reference : entry.component_refs) {
            const LibraryComponent* library = find_library_component(reference.name);
            if (library == nullptr || reference.id != library->id) {
                continue;
            }
            if (std::find(referenced_library.begin(), referenced_library.end(), reference.name) ==
                referenced_library.end()) {
                referenced_library.push_back(reference.name);
            }
        }
    }
    std::sort(referenced_library.begin(), referenced_library.end());
    for (const std::string& name : referenced_library) {
        const bool declared =
            std::any_of(lower.components.begin(), lower.components.end(),
                        [&](const ir::Component& component) { return component.name == name; });
        if (!declared) {
            lower.components.push_back(make_library_component(*find_library_component(name)));
        }
    }
    lower.stylesheets = state.stylesheets;
    std::sort(lower.stylesheets.begin(), lower.stylesheets.end(),
              [](const ir::StylesheetRef& left, const ir::StylesheetRef& right) {
                  return left.path < right.path;
              });
    // ir-v1 section 11: components sorted by source path, then name; bindings sorted by id.
    std::sort(lower.components.begin(), lower.components.end(),
              [](const ir::Component& left, const ir::Component& right) {
                  if (left.source.file != right.source.file) {
                      return left.source.file < right.source.file;
                  }
                  return left.name < right.name;
              });
    std::sort(lower.bindings.begin(), lower.bindings.end(),
              [](const ir::Binding& left, const ir::Binding& right) { return left.id < right.id; });
    return lower;
}

ModuleLoweringResult lower_files(std::span<const ModuleSource> sources,
                                 std::string_view module_name,
                                 std::span<const PackageRoot> packages,
                                 std::span<const StylesheetFile> stylesheets) {
    ProjectState state;
    state.files.resize(sources.size());
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (sources[i].module == nullptr || sources[i].analysis == nullptr ||
            sources[i].source_map == nullptr) {
            state.error =
                make_diagnostic(sources[i], "TESSERA_IR_MALFORMED",
                                "a module source needs its module, analysis and map", jsx::Span{});
            return std::unexpected(NodeLoweringErrors{*state.error});
        }
        state.files[i].source = &sources[i];
    }
    for (std::size_t i = 0; i < sources.size(); ++i) {
        for (std::size_t j = i + 1; j < sources.size(); ++j) {
            if (sources[i].file == sources[j].file) {
                state.error = make_diagnostic(
                    sources[i], "TESSERA_IR_MALFORMED",
                    "the same file is lowered twice: " + std::string(sources[i].file), jsx::Span{});
                return std::unexpected(NodeLoweringErrors{*state.error});
            }
        }
    }
    // Deterministic lowering order: file path. Diagnostics then do not depend on caller order.
    state.order.resize(sources.size());
    for (std::size_t i = 0; i < sources.size(); ++i) {
        state.order[i] = i;
    }
    std::sort(state.order.begin(), state.order.end(), [&](std::size_t left, std::size_t right) {
        return sources[left].file < sources[right].file;
    });
    for (const std::size_t index : state.order) {
        if (!resolve_imports(state, index, packages, stylesheets)) {
            return std::unexpected(NodeLoweringErrors{*state.error});
        }
    }
    // A component name is project-wide unique (design decision 3).
    for (std::size_t i = 0; i < state.files.size(); ++i) {
        for (std::size_t j = i + 1; j < state.files.size(); ++j) {
            for (const std::string& name : state.files[i].declared_names) {
                if (std::find(state.files[j].declared_names.begin(),
                              state.files[j].declared_names.end(),
                              name) != state.files[j].declared_names.end()) {
                    state.error = make_diagnostic(
                        *state.files[i].source, "TESSERA_IR_DUPLICATE_ID",
                        "component `" + name + "` is declared in two files", jsx::Span{});
                    return std::unexpected(NodeLoweringErrors{*state.error});
                }
            }
        }
    }
    std::vector<int> color(state.files.size(), 0);
    for (std::size_t i = 0; i < state.files.size(); ++i) {
        if (has_import_cycle(state.files, i, color)) {
            state.error = make_diagnostic(*state.files[i].source, "TESSERA_IR_UNSUPPORTED",
                                          "import cycle between project files", jsx::Span{});
            return std::unexpected(NodeLoweringErrors{*state.error});
        }
    }
    std::optional<LoweredModule> module = finish(state, module_name);
    if (!module) {
        return std::unexpected(NodeLoweringErrors{*state.error});
    }
    return std::move(*module);
}

} // namespace

std::string join_project_path(std::string_view base, std::string_view relative) {
    std::string joined(base);
    if (!joined.empty()) {
        joined.push_back('/');
    }
    joined += relative;
    std::vector<std::string_view> parts;
    std::string_view pending = joined;
    std::size_t start = 0;
    while (start <= pending.size()) {
        const std::size_t slash = pending.find('/', start);
        const std::string_view part = pending.substr(
            start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (part == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    std::string out;
    for (const std::string_view part : parts) {
        if (!out.empty()) {
            out.push_back('/');
        }
        out += part;
    }
    return out;
}

ModuleLoweringResult lower_module(const ModuleSource& source, std::string_view module_name) {
    return lower_files(std::span<const ModuleSource>(&source, 1), module_name, {}, {});
}

ModuleLoweringResult lower_project(std::span<const ModuleSource> files,
                                   std::string_view module_name) {
    return lower_files(files, module_name, {}, {});
}

ModuleLoweringResult lower_project(std::span<const ModuleSource> files,
                                   std::string_view module_name,
                                   std::span<const PackageRoot> packages) {
    return lower_files(files, module_name, packages, {});
}

ModuleLoweringResult lower_project(std::span<const ModuleSource> files,
                                   std::string_view module_name,
                                   std::span<const PackageRoot> packages,
                                   std::span<const StylesheetFile> stylesheets) {
    return lower_files(files, module_name, packages, stylesheets);
}

} // namespace tessera::compiler::lowering
