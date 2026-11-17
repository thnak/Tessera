// SPDX-License-Identifier: MIT

#pragma once

#include "scene_layout.hpp"
#include "scene_preview.hpp"
#include "scene_stylesheet.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The stage sequence the preview runs, factored out of `scene_preview.cpp` (issue #167, P3). P2
// produced a scene snapshot; P3 needs the *same laid-out tree* twice more - once recorded as a draw
// list and once painted through an RHI device - and the whole point of these gates is that the
// three consumers see one pipeline, not three copies of it. So the sequence lives here, in the
// CLI's order, and every consumer walks the value it returns.
//
// Deliberately no device, no filesystem, no clock: this is the part that runs natively and in Wasm
// unchanged. Choosing a backend is the caller's job.
namespace tessera::compiler::codegen::cli {

// The text module this stage's measurer parameter names lives one namespace up.
namespace text = tessera::ui::text;

// A laid-out preview scene. Ownership is the point: `ui::Node` borrows its element metadata strings
// from the lowered IR, so `module` is declared before `root` and is therefore still alive when the
// tree is destroyed.
struct PreparedScene {
    // Non-empty when some stage stopped. The tree and sheet are then empty and every consumer
    // reports "no scene" the same way `scene_preview_document()` reports an empty document.
    std::string error;
    // The lowered, identity-assigned IR the tree borrows from.
    tessera::compiler::ir::IrModule module;
    // The root `ui::Node` (id 1) whose child is the component's tree, styled and laid out with
    // absolute boxes written back by `apply_layout`.
    std::unique_ptr<tessera::ui::Node> root;
    // The stylesheet the layout and the paint pass both used: the author's when the options carried
    // one, the built-in `preview_stylesheet()` fallback otherwise (issue #390, decision 2). Kept so
    // a painter resolves the same computed styles the layout did. Optional because
    // `CompiledStyleSheet` has no default constructor: empty exactly when `error` is non-empty.
    std::optional<tessera::ui::style::CompiledStyleSheet> sheet;
    // The author stylesheet's parse problems, with line and column (issue #390). Empty when the
    // preview fell back to the built-in sheet, because a sheet the author never wrote has none.
    //
    // These are REPORTED, never fatal: the parser is tolerant, so a bad rule costs that rule and
    // the frame is still drawn. A caller shows them beside the frame, which is what the issue's
    // "the page's failed/diagnostics area" asks for.
    std::vector<PreviewCssDiagnostic> css_diagnostics;
    // Scene nodes whose style was resolved, and nodes `apply_layout` wrote a box to.
    std::size_t nodes = 0;
    std::size_t applied = 0;
    // The simulated window this tree was laid out against (issue #408): the logical extent, the DPI
    // scale and the `FoldInfo` the options' posture resolved to. A consumer that needs to report or
    // re-derive the fold (the snapshot document, the canvas session) reads it from here instead of
    // rebuilding it, so it cannot disagree with the pass that produced the boxes.
    tessera::hal::WindowMetrics metrics;
};

// Runs parse -> analyse -> lower -> assign identity -> pick component -> evaluate state -> build
// the scene tree -> resolve computed style -> Yoga layout -> apply bounds for `text`, exactly as
// `scene_preview_document()` does (they share this function, so they cannot drift).
//
// `measurer` selects the text measurement seam, and it is the one thing about this sequence a
// caller with a font needs to supply: null keeps the deterministic `StubTextMeasurer` (fixed 0.5 em
// per code point) that every font-free caller and golden depends on, while a real face measures
// with FreeType metrics and HarfBuzz advances so a text box is sized to the ink that will be
// painted (issue #314). Nothing else about the sequence changes, so a preview with a font and one
// without differ only in where the numbers come from.
[[nodiscard]] PreparedScene
prepare_scene(std::string_view text, const ScenePreviewOptions& options,
              std::string_view component = {},
              const std::shared_ptr<const text::TextMeasurer>& measurer = nullptr);

// `preview_stylesheet()` -- the built-in fallback -- is declared by `scene_stylesheet.hpp`
// (included above), beside the rule that decides when it is used at all (issue #390, decision 2).

} // namespace tessera::compiler::codegen::cli
