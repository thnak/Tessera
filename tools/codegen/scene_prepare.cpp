// SPDX-License-Identifier: MIT

#include "scene_build.hpp"
#include "scene_layout.hpp"
#include "scene_prepare.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/rules/analyze.hpp"
#include "tessera/ui/layout/layout_engine.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::codegen::cli {
namespace {

namespace eval = tessera::compiler::eval;
namespace ids = tessera::compiler::ids;
namespace ir = tessera::compiler::ir;
namespace jsx = tessera::compiler::jsx;
namespace lowering = tessera::compiler::lowering;
namespace rules = tessera::compiler::rules;
namespace style = tessera::ui::style;
namespace ui = tessera::ui;

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

PreparedScene failed(std::string reason) {
    PreparedScene scene;
    scene.error = std::move(reason);
    return scene;
}

} // namespace

std::string_view preview_stylesheet() noexcept {
    // Sizes *and* paint. The sizes stop every box collapsing to zero, which is what P2's layout
    // needed; the colours are what make P3's render comparison mean anything - a sheet that painted
    // nothing would leave both images a single clear colour and a "within tolerance" verdict would
    // pass while proving nothing (the same anti-vacuity rule the snapshot gate applies to node
    // counts). The palette is the app window's own dark surface so a preview reads as the product.
    //
    // The `:hover`/`:active` rules are #388's "visible result without user CSS": pointer input on
    // the canvas has to be observable before author stylesheets exist (#390 replaces this sheet
    // wholesale). They are the *only* reason the sheet mentions pseudo-classes, and they match
    // nothing while no pointer is over the scene, so every static scene - and therefore every
    // existing golden - resolves to exactly the colours it did before.
    return "View { display: flex; flex-direction: column; background-color: #101a2c; }\n"
           "Text { display: block; height: 24px; font-size: 16px; color: #e6edf7; }\n"
           "Button { width: 120px; height: 32px; background-color: #2d6cdf; "
           "border-radius: 6px; }\n"
           "Button:hover { background-color: #3d7def; }\n"
           "Button:active { background-color: #1f4fae; }\n"
           "TextInput { width: 180px; height: 28px; background-color: #1b2740; }\n"
           "TextInput:hover { background-color: #24365a; }\n"
           "TextArea { width: 180px; height: 64px; background-color: #1b2740; }\n"
           "TextArea:hover { background-color: #24365a; }\n"
           "Image { width: 64px; height: 64px; background-color: #1b2740; }\n"
           "Spacer { height: 8px; }\n"
           "Scroll { width: 200px; height: 80px; background-color: #16203473; }\n";
}

PreparedScene prepare_scene(std::string_view text, const ScenePreviewOptions& options,
                            std::string_view component,
                            const std::shared_ptr<const text::TextMeasurer>& measurer) {
    // Parse, then analyse: the CLI stops at the first stage that fails, so the preview produces no
    // tree for the same inputs it would refuse to compile.
    const jsx::ModuleParseResult parsed = jsx::parse_module(text);
    if (parsed.root == nullptr || parsed.has_errors()) {
        return failed("the source does not parse");
    }
    const jsx::SourceMap map(text);
    const rules::AnalyzedModule analyzed = rules::analyze_module(*parsed.root, map);
    if (analyzed.has_errors()) {
        return failed("the source does not analyse");
    }

    const std::string file = normalize_path(options.file.empty() ? "ui/Preview.tsx" : options.file);
    const lowering::ModuleSource source{file, parsed.root.get(), &analyzed, &map};
    auto lowered = lowering::lower_project(std::span<const lowering::ModuleSource>(&source, 1),
                                           module_name_for(file));
    if (!lowered) {
        return failed("the source does not lower");
    }
    PreparedScene scene;
    scene.module.name = lowered->name;
    scene.module.components = std::move(lowered->components);
    scene.module.bindings = std::move(lowered->bindings);
    scene.module.stylesheets = std::move(lowered->stylesheets);

    // The page passes its salt (the project namespace it shows), so the ids in the document are the
    // ones the native build of the same source would assign. An empty salt leaves the identity
    // unassigned, which is what an unsalted CLI run does; the snapshot then reports `uiId` 0 for
    // every node, so a caller that cares about identity passes a salt.
    if (!options.salt.empty()) {
        if (!ids::assign_identity(scene.module, options.salt)) {
            return failed("the source's ids could not be assigned");
        }
    }

    const ir::Component* chosen = nullptr;
    for (const ir::Component& candidate : scene.module.components) {
        if (component.empty() || candidate.name == component) {
            chosen = &candidate;
            break;
        }
    }
    if (chosen == nullptr) {
        return failed("the source declares no such component");
    }

    scene.root = std::make_unique<ui::Node>(1);
    eval::Scope scope;
    const SceneBuildResult built = build_scene(*chosen, *scene.root, scope);
    if (!built.error.empty()) {
        return failed(built.error);
    }

    scene.sheet = style::compile_stylesheet(preview_stylesheet());
    ui::layout::LayoutEngine engine;
    const SceneLayoutResult laid_out =
        layout_scene(*scene.root, *scene.sheet, engine, options.width, options.height,
                     options.dpi_scale, measurer);
    if (!laid_out.error.empty()) {
        return failed(laid_out.error);
    }
    scene.nodes = laid_out.nodes;
    scene.applied = laid_out.applied;
    return scene;
}

} // namespace tessera::compiler::codegen::cli
