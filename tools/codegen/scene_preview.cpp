// SPDX-License-Identifier: MIT

#include "scene_build.hpp"
#include "scene_layout.hpp"
#include "scene_preview.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/rules/analyze.hpp"
#include "tessera/ui/automation/scene_snapshot.hpp"
#include "tessera/ui/automation/snapshot_json.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::cli {
namespace {

namespace eval = tessera::compiler::eval;
namespace ids = tessera::compiler::ids;
namespace ir = tessera::compiler::ir;
namespace jsx = tessera::compiler::jsx;
namespace lowering = tessera::compiler::lowering;
namespace rules = tessera::compiler::rules;
namespace ui = tessera::ui;

// The document for a run that produced no tree. It is a *valid* document of the same version with
// an empty `nodes` array, so a consumer parses one shape, and the parity gate's minimum-node-count
// guard is what stops "both sides empty" from passing.
std::string empty_document(std::uint64_t sequence) {
    ui::automation::UiSnapshot snapshot;
    snapshot.sequence = sequence;
    return ui::automation::snapshot_to_json(snapshot);
}

// The module namespace a single in-memory file implies, matching the CLI's derivation (a lowercased
// identifier-shaped file stem), so the two runs agree on every string that can reach a document.
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

// The stylesheet the preview lays out with. The page has one buffer and no project, so there is no
// CSS to read: the sheet is the preview's own default rules for the intrinsic element kinds, which
// give the nodes a size instead of collapsing every box to zero. A custom property or class in the
// sample is simply unmatched, exactly as it would be in a project that ships no CSS.
std::string_view preview_stylesheet() {
    return "View { display: flex; flex-direction: column; }\n"
           "Text { display: block; height: 24px; font-size: 16px; }\n"
           "Button { width: 120px; height: 32px; }\n"
           "TextInput { width: 180px; height: 28px; }\n"
           "TextArea { width: 180px; height: 64px; }\n"
           "Image { width: 64px; height: 64px; }\n"
           "Spacer { height: 8px; }\n"
           "Scroll { width: 200px; height: 80px; }\n";
}

// The source path as the ids and the document spell it: POSIX separators, as
// docs/specs/automation-ids-v1.md section 2.1 requires of a source path. The salt and this path are
// both inputs to every `uiId`, so a `\` from a Windows command line would otherwise give a document
// whose ids no Linux run could reproduce — and the parity gate compares the two.
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

} // namespace

std::string scene_preview_document(std::string_view text, const ScenePreviewOptions& options,
                                   std::string_view component) {
    // Parse, then analyse: the CLI stops at the first stage that fails, so the preview produces no
    // tree for the same inputs it would refuse to compile.
    const jsx::ModuleParseResult parsed = jsx::parse_module(text);
    if (parsed.root == nullptr || parsed.has_errors()) {
        return empty_document(0);
    }
    const jsx::SourceMap map(text);
    const rules::AnalyzedModule analyzed = rules::analyze_module(*parsed.root, map);
    if (analyzed.has_errors()) {
        return empty_document(0);
    }

    const std::string file = normalize_path(options.file.empty() ? "ui/Preview.tsx" : options.file);
    const lowering::ModuleSource source{file, parsed.root.get(), &analyzed, &map};
    auto lowered = lowering::lower_project(std::span<const lowering::ModuleSource>(&source, 1),
                                           module_name_for(file));
    if (!lowered) {
        return empty_document(0);
    }
    ir::IrModule module;
    module.name = lowered->name;
    module.components = std::move(lowered->components);
    module.bindings = std::move(lowered->bindings);
    module.stylesheets = std::move(lowered->stylesheets);

    // The page passes its salt (the project namespace it shows), so the ids in the document are the
    // ones the native build of the same source would assign. An empty salt leaves the identity
    // unassigned, which is what an unsalted CLI run does; the snapshot then reports `uiId` 0 for
    // every node, so a caller that cares about identity passes a salt.
    if (!options.salt.empty()) {
        if (!ids::assign_identity(module, options.salt)) {
            return empty_document(0);
        }
    }

    const ir::Component* chosen = nullptr;
    for (const ir::Component& candidate : module.components) {
        if (component.empty() || candidate.name == component) {
            chosen = &candidate;
            break;
        }
    }
    if (chosen == nullptr) {
        return empty_document(0);
    }

    ui::Node root(1);
    eval::Scope scope;
    const SceneBuildResult built = build_scene(*chosen, root, scope);
    if (!built.error.empty()) {
        return empty_document(0);
    }

    const ui::style::CompiledStyleSheet sheet = ui::style::compile_stylesheet(preview_stylesheet());
    ui::layout::LayoutEngine engine;
    const SceneLayoutResult laid_out =
        layout_scene(root, sheet, engine, options.width, options.height, options.dpi_scale);
    if (!laid_out.error.empty()) {
        return empty_document(0);
    }

    // The canonical document, from the walk the native harness uses (issue #166): one
    // implementation of the artifact, so the parity gate compares the pipeline and not two
    // serializers.
    ui::automation::SceneSnapshotOptions snapshot_options;
    snapshot_options.snapshot.sequence = 0;
    auto snapshot = ui::automation::snapshot_scene(root, snapshot_options);
    if (!snapshot.has_value()) {
        return empty_document(0);
    }
    return ui::automation::snapshot_to_json(*snapshot);
}

} // namespace tessera::compiler::codegen::cli
