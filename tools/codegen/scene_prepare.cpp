// SPDX-License-Identifier: MIT

#include "scene_build.hpp"
#include "scene_evaluate.hpp"
#include "scene_layout.hpp"
#include "scene_prepare.hpp"
#include "scene_stylesheet.hpp"
#include "tessera/ui/layout/layout_engine.hpp"

#include <memory>
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
    // Parse -> analyse -> lower -> identity -> pick component, through the ONE implementation of
    // that sequence (`scene_evaluate.cpp`). The snapshot, the interaction session and this stage
    // tree all reach the same function, so "the source does not parse" cannot mean three slightly
    // different things -- and the author stylesheet's virtual file is resolved there, once (issue
    // #390).
    auto evaluated = evaluate_source(text, options.file, options.salt, component,
                                     options.stylesheet, options.stylesheet_file);
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

    // Decision 2 (issue #390): the author's sheet REPLACES the built-in one.
    // `compile_preview_stylesheet()` is the single place that decides, so the snapshot, the native
    // frame and the browser session cannot disagree about which sheet a source was laid out
    // against. The diagnostics come from the same call, so a rule the sheet reports is a rule
    // layout also saw.
    scene.sheet = compile_preview_stylesheet(options.stylesheet);
    scene.css_diagnostics =
        author_stylesheet_diagnostics(options.stylesheet, options.stylesheet_file);
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
