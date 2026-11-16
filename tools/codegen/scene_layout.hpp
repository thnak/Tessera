// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cstddef>
#include <memory>
#include <string>

// Style + layout for the in-memory preview (issue #166). It runs the engine's own stages over the
// tree `scene_build.cpp` produced: computed style through the compiled stylesheet, then Yoga
// through `LayoutEngine`, then `apply_layout` writes absolute boxes back onto the scene nodes.
// There is no GPU, no device and no RHI involved; the null-RHI claim of that slice is exactly that
// the path stops at the scene's bounds and never reaches for a render target.
//
// **Why the measurer is a parameter.** Layout sizes a text box from whatever `TextMeasurer` it was
// constructed with, and the engine's default is `StubTextMeasurer`, whose advance is a fixed 0.5 em
// per code point. That is right for a font-free path and wrong for anything that then *paints* real
// glyphs: a stub-sized box disagrees with its painted ink by a string-dependent amount (issue
// #314). So the font-free preview passes nothing (stub boxes, `text_skipped` counted) and a preview
// with font bytes passes the face's own measurer, which is the same seam the app window installs.
// One call site, one parameter, no second layout path.
namespace tessera::compiler::codegen::cli {

// The text module and the scene/style modules this stage names live one namespace up.
namespace text = tessera::ui::text;

struct SceneLayoutResult {
    std::string error;
    std::size_t nodes = 0;   // scene nodes whose style was resolved
    std::size_t applied = 0; // nodes whose box layout wrote
};

// Sizes `root` to `width` x `height` (inline style, the way `AppHost` sizes a window root) and lays
// the subtree out. `sheet` must be alive for the duration of the call only, and so must `measurer`.
// The layout viewport is the same rectangle, so the two cannot disagree about the window. A null
// `measurer` selects the deterministic stub the font-free preview has always used.
[[nodiscard]] SceneLayoutResult
layout_scene(ui::Node& root, const ui::style::CompiledStyleSheet& sheet,
             ui::layout::LayoutEngine& engine, float width, float height, float dpi_scale,
             const std::shared_ptr<const text::TextMeasurer>& measurer = nullptr);

} // namespace tessera::compiler::codegen::cli
