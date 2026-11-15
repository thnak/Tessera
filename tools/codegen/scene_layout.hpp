// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstddef>
#include <string>

// Style + layout for the in-memory preview (issue #166). It runs the engine's own stages over the
// tree `scene_build.cpp` produced: computed style through the compiled stylesheet, then Yoga
// through `LayoutEngine`, then `apply_layout` writes absolute boxes back onto the scene nodes.
// There is no GPU, no device and no RHI involved; the null-RHI claim of this slice is exactly that
// the path stops at the scene's bounds and never reaches for a render target.
namespace tessera::compiler::codegen::cli {

struct SceneLayoutResult {
    std::string error;
    std::size_t nodes = 0;   // scene nodes whose style was resolved
    std::size_t applied = 0; // nodes whose box layout wrote
};

// Sizes `root` to `width` x `height` (inline style, the way `AppHost` sizes a window root) and lays
// the subtree out. `sheet` must be alive for the duration of the call only. The layout viewport is
// the same rectangle, so the two cannot disagree about the window.
[[nodiscard]] SceneLayoutResult layout_scene(ui::Node& root,
                                             const ui::style::CompiledStyleSheet& sheet,
                                             ui::layout::LayoutEngine& engine, float width,
                                             float height, float dpi_scale);

} // namespace tessera::compiler::codegen::cli
