// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/fold_posture.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

// Style + layout for the in-memory preview (issue #166). It runs the engine's own stages over the
// tree `scene_build.cpp` produced: computed style through the compiled stylesheet, then Yoga
// through `LayoutEngine`, then `apply_layout` writes absolute boxes back onto the scene nodes.
// There is no GPU, no device and no RHI involved; the null-RHI claim of this slice is exactly that
// the path stops at the scene's bounds and never reaches for a render target.
//
// **The simulated device (issue #408).** The preview has no OS window, so it has no
// `hal::WindowMetrics` of its own; before this slice it laid a tree out with no window metrics at
// all, and a `SafeArea` therefore resolved to nothing. It now builds the ONE snapshot the engine
// reads - `WindowMetrics::logical_width/height/dpi_scale` plus `WindowMetrics::fold` from the
// chosen fold posture - and hands it to `ui::style::apply_safe_area_style()`, which is the same
// function `AppHost::build_layout_input()` calls. So an inset, a hinge band and the full-occlusion
// rule are the engine's arithmetic, not a preview approximation of it.
namespace tessera::compiler::codegen::cli {

// Everything a preview layout needs to describe the simulated window. Defaults are the P2/P3
// viewport with no fold, which is byte-identical to what every existing preview golden and parity
// gate expects: a `FoldPosture::None` snapshot contributes no insets, so no `SafeArea` node's
// padding changes when a caller does not ask for a fold.
struct SceneMetrics {
    float width = 320.0f;
    float height = 240.0f;
    float dpi_scale = 1.0f;
    hal::FoldPosture posture = hal::FoldPosture::None;

    [[nodiscard]] bool operator==(const SceneMetrics&) const = default;
};

// The `WindowMetrics` a preview lays out against: the logical extent, the DPI scale and the
// `FoldInfo` the posture resolves to (`hal::fold_info_for`). Deterministic and free of any OS,
// clock or sensor input, so two runs of one posture produce the same snapshot.
//
// The extent is rounded the way the HAL reports one (whole logical pixels), because
// `hal::resolve_safe_area` reads `logical_width`/`logical_height` to place the hinge band and a
// fractional extent would make the band fractional too.
[[nodiscard]] hal::WindowMetrics simulated_metrics(const SceneMetrics& metrics) noexcept;

struct SceneLayoutResult {
    std::string error;
    std::size_t nodes = 0;   // scene nodes whose style was resolved
    std::size_t applied = 0; // nodes whose box layout wrote
    // The window metrics this pass laid out against, so a caller (the snapshot driver, the canvas
    // session) can report or reuse exactly what the tree was sized to.
    hal::WindowMetrics metrics;
};

// Sizes `root` to `metrics.width` x `metrics.height` (inline style, the way `AppHost` sizes a
// window root) and lays the subtree out. `sheet` must be alive for the duration of the call only.
// The layout viewport is the same rectangle, so the two cannot disagree about the window.
[[nodiscard]] SceneLayoutResult layout_scene(ui::Node& root,
                                             const ui::style::CompiledStyleSheet& sheet,
                                             ui::layout::LayoutEngine& engine,
                                             const SceneMetrics& metrics);

} // namespace tessera::compiler::codegen::cli
