// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/fold_posture.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// Style + layout for the in-memory preview (issue #166). It runs the engine's own stages over the
// tree `scene_build.cpp` produced: computed style through the compiled stylesheet, then Yoga
// through `LayoutEngine`, then `apply_layout` writes absolute boxes back onto the scene nodes.
// There is no GPU, no device and no RHI involved; the null-RHI claim of that slice is exactly that
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
//
// **The skin's safe area is the same two fields a device reports** (issue #171's skins slice,
// `designer-studio-v1.md` section 5.1: "its insets are passed to the viewer as `safeAreas` in
// `viewport.set`, exactly as `WindowMetrics::insets` would arrive from the HAL"). `insets` is what
// `hal::resolve_safe_area()` raises the `SafeArea` padding from and `cutouts` is the rectangle a
// cutout overlay reads; both default to zero/empty, which is the desktop answer and the pre-#171
// behaviour, so a caller that never sets them lays out exactly as before.
struct SceneMetrics {
    float width = 320.0f;
    float height = 240.0f;
    float dpi_scale = 1.0f;
    hal::FoldPosture posture = hal::FoldPosture::None;
    hal::WindowInsets insets;
    hal::CutoutInfo cutouts;

    // Written out rather than `= default`: `WindowInsets` and `CutoutInfo` are plain HAL value
    // types with no `operator==` of their own, and adding one to the public HAL header would be a
    // public API change for a comparison only the preview needs. Comparing the fields here keeps
    // the "did any layout input move?" test in one place, which is what makes a skin change with an
    // unchanged size still cost exactly one layout pass.
    [[nodiscard]] bool operator==(const SceneMetrics& other) const noexcept;
};

// Whether two HAL value types are equal. `WindowInsets` and `CutoutInfo` have no `operator==` of
// their own, and adding one would be a public HAL API change for a comparison only the preview
// needs. The preview's "did any layout input move?" test therefore compares them here, once, so
// `SceneMetrics::operator==` and the sessions' change detection cannot disagree about what "the
// same safe area" means.
[[nodiscard]] bool same_insets(const hal::WindowInsets& a, const hal::WindowInsets& b) noexcept;
[[nodiscard]] bool same_cutouts(const hal::CutoutInfo& a, const hal::CutoutInfo& b) noexcept;

// The `WindowMetrics` a preview lays out against: the logical extent, the DPI scale, the
// `FoldInfo` the posture resolves to (`hal::fold_info_for`), and the skin's insets and cutout
// rectangles. Deterministic and free of any OS, clock or sensor input, so two runs of one posture
// and one skin produce the same snapshot.
//
// The extent is rounded the way the HAL reports one (whole logical pixels), because
// `hal::resolve_safe_area` reads `logical_width`/`logical_height` to place the hinge band and a
// fractional extent would make the band fractional too.
[[nodiscard]] hal::WindowMetrics simulated_metrics(const SceneMetrics& metrics) noexcept;

// The text module and the scene/style modules this stage names live one namespace up.
namespace text = tessera::ui::text;

struct SceneLayoutResult {
    std::string error;
    std::size_t nodes = 0;   // scene nodes whose style was resolved
    std::size_t applied = 0; // nodes whose box layout wrote
    // The window metrics this pass laid out against, so a caller (the snapshot driver, the canvas
    // session) can report or reuse exactly what the tree was sized to.
    hal::WindowMetrics metrics;
};

[[nodiscard]] SceneLayoutResult
layout_scene(ui::Node& root, const ui::style::CompiledStyleSheet& sheet,
             ui::layout::LayoutEngine& engine, const SceneMetrics& metrics,
             std::shared_ptr<const text::TextMeasurer> measurer = nullptr);

} // namespace tessera::compiler::codegen::cli
