// SPDX-License-Identifier: MIT

#include "scene_evaluate.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/rules/analyze.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::cli {
namespace {

namespace ids = tessera::compiler::ids;
namespace jsx = tessera::compiler::jsx;
namespace lowering = tessera::compiler::lowering;
namespace rules = tessera::compiler::rules;

// One parsed project file. Heap-allocated and never moved: the map views the text, the analysis
// views the parse tree, and `lowering::ModuleSource` holds pointers to all three.
struct Unit {
    std::string path;
    jsx::ModuleParseResult parsed;
    std::unique_ptr<jsx::SourceMap> map;
    rules::AnalyzedModule analyzed;
};

std::string directory_of(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
}

std::string without_source_extension(std::string_view path) {
    for (const std::string_view extension : {std::string_view(".tsx"), std::string_view(".jsx")}) {
        if (path.size() > extension.size() &&
            path.compare(path.size() - extension.size(), extension.size(), extension) == 0) {
            return std::string(path.substr(0, path.size() - extension.size()));
        }
    }
    return std::string(path);
}

const ProjectFile* find_file(std::span<const ProjectFile> files, std::string_view path) {
    for (const ProjectFile& file : files) {
        if (normalize_path(file.path) == path) {
            return &file;
        }
    }
    return nullptr;
}

// The source file an extension-less relative import names, or null. Mirrors the lowering's rule
// (`find_project_file`: the path without its extension), so the walk reaches exactly the files the
// lowering will ask for.
const ProjectFile* find_import_target(std::span<const ProjectFile> sources, std::string_view key) {
    for (const ProjectFile& file : sources) {
        if (without_source_extension(normalize_path(file.path)) == key) {
            return &file;
        }
    }
    return nullptr;
}

bool is_relative(std::string_view specifier) {
    return specifier.rfind("./", 0) == 0 || specifier.rfind("../", 0) == 0;
}

struct Walk {
    const ProjectInput& input;
    std::vector<std::unique_ptr<Unit>> units;
    std::vector<SheetUse> sheets;
    std::optional<EvaluateError> error;

    Unit* visit(const ProjectFile& file) {
        const std::string path = normalize_path(file.path);
        for (const std::unique_ptr<Unit>& known : units) {
            if (known->path == path) {
                return known.get();
            }
        }
        auto unit = std::make_unique<Unit>();
        unit->path = path;
        unit->parsed = jsx::parse_module(file.text);
        if (unit->parsed.root == nullptr || unit->parsed.has_errors()) {
            error = EvaluateError(EvaluateErrorCode::File, path + " does not parse", path);
            return nullptr;
        }
        unit->map = std::make_unique<jsx::SourceMap>(file.text);
        unit->analyzed = rules::analyze_module(*unit->parsed.root, *unit->map);
        if (unit->analyzed.has_errors()) {
            error = EvaluateError(EvaluateErrorCode::File, path + " does not analyse", path);
            return nullptr;
        }
        Unit* raw = unit.get();
        units.push_back(std::move(unit));
        // Depth-first in source order: a sheet lands where it is first imported, which is the
        // cascade order of the web's import graph and of the native build.
        for (const jsx::ModuleItem& item : raw->parsed.root->items) {
            if (item.kind != jsx::ItemKind::Import && item.kind != jsx::ItemKind::ReExport) {
                continue;
            }
            if (item.source.empty() || !is_relative(item.source)) {
                continue; // `tessera`, `react`, `tessera-ui`: built in, not project files
            }
            const std::string key = lowering::join_project_path(directory_of(path), item.source);
            if (item.kind == jsx::ItemKind::Import &&
                (item.side_effect || !item.css_module_binding.empty())) {
                if (!add_sheet(key, path)) {
                    return nullptr;
                }
                continue;
            }
            const ProjectFile* target = find_import_target(input.sources, key);
            if (target == nullptr) {
                error = EvaluateError(EvaluateErrorCode::File,
                                      path + ": import `" + item.source +
                                          "` does not name a file of this project",
                                      path);
                return nullptr;
            }
            if (visit(*target) == nullptr) {
                return nullptr;
            }
        }
        return raw;
    }

    bool add_sheet(const std::string& key, const std::string& importer) {
        for (const SheetUse& known : sheets) {
            if (known.path == key) {
                return true;
            }
        }
        const ProjectFile* sheet = find_file(input.stylesheets, key);
        if (sheet == nullptr) {
            error = EvaluateError(EvaluateErrorCode::File,
                                  importer + ": the stylesheet `" + key + "` is not in the project",
                                  importer);
            return false;
        }
        sheets.push_back(SheetUse{key, std::string(sheet->text)});
        return true;
    }
};

} // namespace

std::string module_name_for(std::string_view file) {
    const std::size_t slash = file.find_last_of('/');
    std::string stem(file.substr(slash == std::string_view::npos ? 0 : slash + 1));
    const std::size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos && dot != 0) {
        stem.resize(dot);
    }
    std::string name;
    for (const char c : stem) {
        const auto ch = static_cast<unsigned char>(c);
        name += ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                 c == '_')
                    ? static_cast<char>(ch >= 'A' && ch <= 'Z' ? c - 'A' + 'a' : c)
                    : '_';
    }
    if (name.empty()) {
        name = "module";
    }
    if (name.front() >= '0' && name.front() <= '9') {
        name.insert(name.begin(), '_');
    }
    return name;
}

std::string normalize_path(std::string_view file) {
    std::string path(file);
    for (char& character : path) {
        if (character == '\\') {
            character = '/';
        }
    }
    while (path.rfind("./", 0) == 0) {
        path.erase(0, 2);
    }
    return path;
}

std::expected<EvaluatedScene, EvaluateError>
evaluate_source(std::string_view text, std::string_view file, std::string_view salt,
                std::string_view component, std::string_view stylesheet,
                std::string_view stylesheet_file) {
    // Parse, then analyse: the CLI stops at the first stage that fails, so the preview produces no
    // tree for the same inputs it would refuse to compile.
    const jsx::ModuleParseResult parsed = jsx::parse_module(text);
    if (parsed.root == nullptr || parsed.has_errors()) {
        return std::unexpected(
            EvaluateError{EvaluateErrorCode::Parse, "the source does not parse"});
    }
    const jsx::SourceMap map(text);
    const rules::AnalyzedModule analyzed = rules::analyze_module(*parsed.root, map);
    if (analyzed.has_errors()) {
        return std::unexpected(
            EvaluateError{EvaluateErrorCode::Analyze, "the source does not analyse"});
    }

    const std::string path = normalize_path(file.empty() ? "ui/Preview.tsx" : file);
    const lowering::ModuleSource source{path, parsed.root.get(), &analyzed, &map};
    // The page's stylesheet textarea is a virtual project file (issue #390): one entry, always
    // present, so `import "./Preview.css";` resolves to the author's own text rather than a missing
    // path. Empty CSS is a valid file with no rules, not an error.
    const std::string css_path =
        normalize_path(stylesheet_file.empty() ? "ui/Preview.css" : stylesheet_file);
    const lowering::StylesheetFile css{css_path, stylesheet};
    auto lowered =
        lowering::lower_project(std::span<const lowering::ModuleSource>(&source, 1),
                                module_name_for(path), std::span<const lowering::PackageRoot>(),
                                std::span<const lowering::StylesheetFile>(&css, 1));
    if (!lowered) {
        return std::unexpected(
            EvaluateError{EvaluateErrorCode::Lower, "the source does not lower"});
    }
    EvaluatedScene evaluated;
    evaluated.module.name = lowered->name;
    evaluated.module.components = std::move(lowered->components);
    evaluated.module.bindings = std::move(lowered->bindings);
    evaluated.module.stylesheets = std::move(lowered->stylesheets);

    // The page passes its salt (the project namespace it shows), so the ids in the document are the
    // ones a native build of the same source would assign.
    if (!salt.empty()) {
        if (auto assigned = ids::assign_identity(evaluated.module, salt); !assigned) {
            return std::unexpected(
                EvaluateError{EvaluateErrorCode::Identity, assigned.error().message});
        }
    }

    for (std::size_t index = 0; index < evaluated.module.components.size(); ++index) {
        if (component.empty() || evaluated.module.components[index].name == component) {
            evaluated.component = index;
            return evaluated;
        }
    }
    return std::unexpected(
        EvaluateError{EvaluateErrorCode::NoComponent, "the source declares no such component"});
}

std::expected<EvaluatedScene, EvaluateError>
evaluate_project(const ProjectInput& input, std::string_view salt, std::string_view component) {
    const std::string entry = normalize_path(input.entry);
    if (entry.empty()) {
        return std::unexpected(
            EvaluateError(EvaluateErrorCode::NoEntry, "the project names no entry file"));
    }
    const ProjectFile* entry_file = find_file(input.sources, entry);
    if (entry_file == nullptr) {
        return std::unexpected(EvaluateError(
            EvaluateErrorCode::NoEntry, "the entry `" + entry + "` is not a file of the project"));
    }

    Walk walk{input, {}, {}, std::nullopt};
    if (walk.visit(*entry_file) == nullptr) {
        return std::unexpected(*walk.error);
    }

    std::vector<lowering::ModuleSource> sources;
    sources.reserve(walk.units.size());
    for (const std::unique_ptr<Unit>& unit : walk.units) {
        sources.push_back(lowering::ModuleSource{unit->path, unit->parsed.root.get(),
                                                 &unit->analyzed, unit->map.get()});
    }
    std::vector<lowering::StylesheetFile> sheet_files;
    sheet_files.reserve(walk.sheets.size());
    for (const SheetUse& sheet : walk.sheets) {
        sheet_files.push_back(lowering::StylesheetFile{sheet.path, sheet.text});
    }
    auto lowered =
        lowering::lower_project(std::span<const lowering::ModuleSource>(sources),
                                module_name_for(entry), std::span<const lowering::PackageRoot>(),
                                std::span<const lowering::StylesheetFile>(sheet_files));
    if (!lowered) {
        return std::unexpected(
            EvaluateError(EvaluateErrorCode::Lower, "the project does not lower"));
    }
    EvaluatedScene evaluated;
    evaluated.module.name = lowered->name;
    evaluated.module.components = std::move(lowered->components);
    evaluated.module.bindings = std::move(lowered->bindings);
    evaluated.module.stylesheets = std::move(lowered->stylesheets);
    evaluated.sheets = std::move(walk.sheets);

    if (!salt.empty()) {
        if (auto assigned = ids::assign_identity(evaluated.module, salt); !assigned) {
            return std::unexpected(
                EvaluateError(EvaluateErrorCode::Identity, assigned.error().message));
        }
    }
    // The component comes from the ENTRY: another file's component is a building block, not the
    // screen. The first match in the entry wins for an empty name, as a one-file source does.
    for (std::size_t index = 0; index < evaluated.module.components.size(); ++index) {
        const ir::Component& candidate = evaluated.module.components[index];
        if (normalize_path(candidate.source.file) != entry) {
            continue;
        }
        if (component.empty() || candidate.name == component) {
            evaluated.component = index;
            return evaluated;
        }
    }
    return std::unexpected(EvaluateError(EvaluateErrorCode::NoComponent,
                                         "the entry `" + entry + "` declares no such component"));
}

} // namespace tessera::compiler::codegen::cli
