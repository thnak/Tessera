// SPDX-License-Identifier: MIT

#include "diagnostic_record.hpp"
#include "scene_build.hpp"
#include "scene_evaluate.hpp"
#include "scene_layout.hpp"
#include "scene_prepare.hpp"
#include "scene_stylesheet.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/style/prepared_stylesheet.hpp"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::codegen::cli {
namespace {

namespace eval = tessera::compiler::eval;
namespace ir = tessera::compiler::ir;
namespace ui = tessera::ui;

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
    // **When this sheet is used (issue #390, owner decision 2).** Only when the project supplies no
    // stylesheet at all: an empty author sheet, a comment-only one, or a caller that never set
    // `ScenePreviewOptions::stylesheet`. Once the author writes any CSS, this sheet is NOT compiled
    // -- it is replaced, never layered under the author's rules. `compile_preview_stylesheet()` in
    // `scene_stylesheet.cpp` is the one place that decides, and `has_author_stylesheet()` is the
    // test.
    //
    // The `:hover`/`:active` rules are #388's "visible result without user CSS": pointer input on
    // the canvas has to be observable in the fallback. They are the *only* reason the sheet
    // mentions pseudo-classes, and they match nothing while no pointer is over the scene, so every
    // static scene - and therefore every existing golden - resolves to exactly the colours it did
    // before.
    return "View { display: flex; flex-direction: column; background-color: #101a2c; }\n"
           "Text { display: block; height: 24px; font-size: 16px; color: #e6edf7; }\n"
           "Button { width: 120px; height: 32px; background-color: #2d6cdf; "
           "border-radius: 6px; }\n"
           "Button:hover { background-color: #3d7def; }\n"
           "Button:active { background-color: #1f4fae; }\n"
           "Button:focus-visible { background-color: #4a86ff; border-width: 2px; "
           "border-color: #ffffff; }\n"
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
    // One file is a project of one: the same core runs, so a single-file caller (the CLIs, the MCP
    // tools, a test) cannot see a different pipeline from the studio's. The stylesheet is a project
    // file like any other and applies only when the source imports it -- which is what a native
    // build of the same file does.
    const std::string file(options.file.empty() ? std::string_view("ui/Preview.tsx")
                                                : options.file);
    const std::array<ProjectFile, 1> sources{{{file, text}}};
    const std::array<ProjectFile, 1> sheets{{{options.stylesheet_file, options.stylesheet}}};
    const ProjectInput project{sources, sheets, file};
    return prepare_scene(project, options, component, measurer);
}

PreparedScene prepare_scene(const ProjectInput& project, const ScenePreviewOptions& options,
                            std::string_view component,
                            const std::shared_ptr<const text::TextMeasurer>& measurer) {
    // Parse -> analyse -> lower -> identity -> pick component, through the ONE implementation of
    // that sequence (`scene_evaluate.cpp`). The snapshot, the interaction session and this stage
    // tree all reach the same function, so "the source does not parse" cannot mean three slightly
    // different things.
    auto evaluated = evaluate_project(project, options.salt, component);
    if (!evaluated) {
        return failed(evaluated.error().detail.empty() ? "the source produced no component"
                                                       : evaluated.error().detail);
    }
    PreparedScene scene;
    scene.module = std::move(evaluated->module);
    // The component is named through `scene.module`, NOT through `*evaluated`: the move above
    // leaves `evaluated->module` empty, so a reference taken from it would index an empty vector.
    if (evaluated->component >= scene.module.components.size()) {
        return failed("the source produced no component");
    }
    const ir::Component& chosen = scene.module.components[evaluated->component];

    scene.root = std::make_unique<ui::Node>(1);
    eval::Scope scope;
    const SceneBuildResult built = build_scene(chosen, *scene.root, scope);
    if (!built.error.empty()) {
        return failed(built.error);
    }

    // The author's sheets REPLACE the built-in one (issue #390, decision 2), and the sheets are the
    // ones the program imports. `prepare_preview_sheets()` is the single place that decides, so the
    // snapshot, the native frame and the browser session cannot disagree about which sheet a source
    // was laid out against; the diagnostics come from the same call, so a rule a sheet reports is a
    // rule layout also saw.
    //
    // The media context is the preview's simulated device (issue #436): the logical viewport this
    // document is laid out at and the DPI scale, so `@media (max-width: 600px)` and the viewport
    // units answer for the rectangle the boxes are in.
    const ui::style::MediaContext media =
        preview_media_context(options.width, options.height, options.dpi_scale);
    PreviewSheets prepared = prepare_preview_sheets(evaluated->sheets);
    scene.sheet = prepared.prepared.resolve(media);
    scene.css_diagnostics = std::move(prepared.diagnostics);
    // Keep the sheets: the resolved sheet's rules view their file names (see `PreparedScene`).
    scene.sheets = std::move(prepared.prepared);
    ui::layout::LayoutEngine engine(measurer);
    SceneMetrics metrics;
    metrics.width = options.width;
    metrics.height = options.height;
    metrics.dpi_scale = options.dpi_scale;
    metrics.posture = options.posture;
    const SceneLayoutResult laid_out =
        layout_scene(*scene.root, *scene.sheet, engine, metrics, measurer);
    if (!laid_out.error.empty()) {
        return failed(laid_out.error);
    }
    scene.nodes = laid_out.nodes;
    scene.applied = laid_out.applied;
    scene.metrics = laid_out.metrics;
    return scene;
}

} // namespace tessera::compiler::codegen::cli
