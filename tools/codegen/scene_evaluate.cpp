// SPDX-License-Identifier: MIT

#include "scene_evaluate.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/rules/analyze.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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

} // namespace tessera::compiler::codegen::cli
