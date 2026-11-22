// SPDX-License-Identifier: MIT

#pragma once

#include "scene_project.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/rules/analyze.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Which files of a project a preview compiles, decided once.
//
// The studio holds a project in memory (`ProjectInput`), and two consumers need to answer "what
// does the entry reach?": `evaluate_project()` (the scene) and `diagnose_project()` (the
// diagnostics document). If each walked the imports itself they would eventually disagree about a
// file, and the page would show a diagnostic for a file the preview never compiled, or a preview of
// a file the diagnostics never read. So the walk is here, header-only so both the diagnostics
// module (which links no scene code) and the scene module use the same bytes of it.
//
// The walk is demand-driven: it parses the entry, then each project file an import names, depth
// first in source order. A file nothing imports is never parsed, which is what keeps a half-typed
// unrelated file from blanking the preview. A sheet lands at its first import, which is the web's
// import-graph cascade order and the native build's.
namespace tessera::compiler::codegen::cli {

// One parsed project file. Heap-allocated and never moved: the map views the text, the analysis
// views the parse tree, and `lowering::ModuleSource` holds pointers to all three.
struct WalkUnit {
    std::string path;
    tessera::compiler::jsx::ModuleParseResult parsed;
    std::unique_ptr<tessera::compiler::jsx::SourceMap> map;
    tessera::compiler::rules::AnalyzedModule analyzed;
};

// Why the walk stopped. `file` is the project file the failure is in.
struct WalkFailure {
    enum class Kind : std::uint8_t { Parse, Analyze, Import, Stylesheet };
    Kind kind = Kind::Parse;
    std::string file;
    std::string message;
};

// POSIX separators and no leading `./` (automation-ids-v1 section 2.1), so a `\` from a Windows
// path does not give a document no Linux run reproduces.
[[nodiscard]] inline std::string normalize_project_path(std::string_view file) {
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

class ProjectWalk {
public:
    explicit ProjectWalk(const ProjectInput& input)
        : input_(input) {}

    // Walks from the entry. False when `failure()` is set; `units()` then holds the files parsed
    // before the stop, and `failed_unit()` the one that failed to parse or analyse, if that was it.
    bool run() {
        const std::string entry = normalize_project_path(input_.entry);
        const ProjectFile* file = find_file(input_.sources, entry);
        if (file == nullptr) {
            return false;
        }
        return visit(*file) != nullptr;
    }

    [[nodiscard]] bool has_entry() const {
        const std::string entry = normalize_project_path(input_.entry);
        return !entry.empty() && find_file(input_.sources, entry) != nullptr;
    }
    [[nodiscard]] const std::optional<WalkFailure>& failure() const noexcept {
        return failure_;
    }
    [[nodiscard]] const std::vector<std::unique_ptr<WalkUnit>>& units() const noexcept {
        return units_;
    }
    [[nodiscard]] const std::vector<SheetUse>& sheets() const noexcept {
        return sheets_;
    }
    // The unit whose parse or analysis failed; null for an import or stylesheet failure.
    [[nodiscard]] const WalkUnit* failed_unit() const noexcept {
        return failed_.get();
    }
    std::vector<SheetUse> take_sheets() {
        return std::move(sheets_);
    }

private:
    static const ProjectFile* find_file(std::span<const ProjectFile> files, std::string_view path) {
        for (const ProjectFile& file : files) {
            if (normalize_project_path(file.path) == path) {
                return &file;
            }
        }
        return nullptr;
    }

    static std::string directory_of(std::string_view path) {
        const std::size_t slash = path.rfind('/');
        return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
    }

    static std::string without_source_extension(std::string_view path) {
        for (const std::string_view extension :
             {std::string_view(".tsx"), std::string_view(".jsx")}) {
            if (path.size() > extension.size() &&
                path.compare(path.size() - extension.size(), extension.size(), extension) == 0) {
                return std::string(path.substr(0, path.size() - extension.size()));
            }
        }
        return std::string(path);
    }

    // The source file an extension-less relative import names, or null. Mirrors the lowering's rule
    // (`find_project_file`: the path without its extension), so the walk reaches exactly the files
    // the lowering will ask for.
    static const ProjectFile* find_import_target(std::span<const ProjectFile> sources,
                                                 std::string_view key) {
        for (const ProjectFile& file : sources) {
            if (without_source_extension(normalize_project_path(file.path)) == key) {
                return &file;
            }
        }
        return nullptr;
    }

    static bool is_relative(std::string_view specifier) {
        return specifier.rfind("./", 0) == 0 || specifier.rfind("../", 0) == 0;
    }

    void fail(WalkFailure::Kind kind, std::string file, std::string message) {
        if (!failure_) {
            failure_ = WalkFailure{kind, std::move(file), std::move(message)};
        }
    }

    WalkUnit* visit(const ProjectFile& file) {
        namespace jsx = tessera::compiler::jsx;
        namespace lowering = tessera::compiler::lowering;
        namespace rules = tessera::compiler::rules;
        const std::string path = normalize_project_path(file.path);
        for (const std::unique_ptr<WalkUnit>& known : units_) {
            if (known->path == path) {
                return known.get();
            }
        }
        auto unit = std::make_unique<WalkUnit>();
        unit->path = path;
        unit->parsed = jsx::parse_module(file.text);
        if (unit->parsed.root == nullptr || unit->parsed.has_errors()) {
            fail(WalkFailure::Kind::Parse, path, path + " does not parse");
            failed_ = std::move(unit);
            return nullptr;
        }
        unit->map = std::make_unique<jsx::SourceMap>(file.text);
        unit->analyzed = rules::analyze_module(*unit->parsed.root, *unit->map);
        if (unit->analyzed.has_errors()) {
            fail(WalkFailure::Kind::Analyze, path, path + " does not analyse");
            failed_ = std::move(unit);
            return nullptr;
        }
        WalkUnit* raw = unit.get();
        units_.push_back(std::move(unit));
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
            const ProjectFile* target = find_import_target(input_.sources, key);
            if (target == nullptr) {
                fail(WalkFailure::Kind::Import, path,
                     path + ": import `" + item.source + "` does not name a file of this project");
                return nullptr;
            }
            if (visit(*target) == nullptr) {
                return nullptr;
            }
        }
        return raw;
    }

    bool add_sheet(const std::string& key, const std::string& importer) {
        for (const SheetUse& known : sheets_) {
            if (known.path == key) {
                return true;
            }
        }
        const ProjectFile* sheet = find_file(input_.stylesheets, key);
        if (sheet == nullptr) {
            fail(WalkFailure::Kind::Stylesheet, importer,
                 importer + ": the stylesheet `" + key + "` is not in the project");
            return false;
        }
        sheets_.push_back(SheetUse{key, std::string(sheet->text)});
        return true;
    }

    const ProjectInput& input_;
    std::vector<std::unique_ptr<WalkUnit>> units_;
    std::vector<SheetUse> sheets_;
    std::unique_ptr<WalkUnit> failed_;
    std::optional<WalkFailure> failure_;
};

} // namespace tessera::compiler::codegen::cli
