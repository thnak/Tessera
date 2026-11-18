// SPDX-License-Identifier: MIT

#include "project_walk.hpp"
#include "scene_evaluate.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/rules/analyze.hpp"

#include <array>
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
    return normalize_project_path(file);
}

std::expected<EvaluatedScene, EvaluateError>
evaluate_source(std::string_view text, std::string_view file, std::string_view salt,
                std::string_view component, std::string_view stylesheet,
                std::string_view stylesheet_file) {
    // One file is a project of one: this is `evaluate_project()` over a single source, so the two
    // entry points cannot disagree about what a program is. The stylesheet is a project file at
    // `stylesheet_file` and applies only when the source imports it.
    const std::string path = normalize_path(file.empty() ? "ui/Preview.tsx" : file);
    const std::string css_path =
        normalize_path(stylesheet_file.empty() ? "ui/Preview.css" : stylesheet_file);
    const std::array<ProjectFile, 1> sources{{{path, text}}};
    const std::array<ProjectFile, 1> sheets{{{css_path, stylesheet}}};
    return evaluate_project(ProjectInput{sources, sheets, path}, salt, component);
}

std::expected<EvaluatedScene, EvaluateError>
evaluate_project(const ProjectInput& input, std::string_view salt, std::string_view component) {
    const std::string entry = normalize_path(input.entry);
    if (entry.empty()) {
        return std::unexpected(
            EvaluateError(EvaluateErrorCode::NoEntry, "the project names no entry file"));
    }
    ProjectWalk walk(input);
    if (!walk.has_entry()) {
        return std::unexpected(EvaluateError(
            EvaluateErrorCode::NoEntry, "the entry `" + entry + "` is not a file of the project"));
    }
    if (!walk.run()) {
        const WalkFailure& failure = *walk.failure();
        return std::unexpected(
            EvaluateError(EvaluateErrorCode::File, failure.message, failure.file));
    }

    std::vector<lowering::ModuleSource> sources;
    sources.reserve(walk.units().size());
    for (const std::unique_ptr<WalkUnit>& unit : walk.units()) {
        sources.push_back(lowering::ModuleSource{unit->path, unit->parsed.root.get(),
                                                 &unit->analyzed, unit->map.get()});
    }
    std::vector<lowering::StylesheetFile> sheet_files;
    sheet_files.reserve(walk.sheets().size());
    for (const SheetUse& sheet : walk.sheets()) {
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
    evaluated.sheets = walk.take_sheets();

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
