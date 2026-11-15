// SPDX-License-Identifier: MIT

#pragma once

#include "scene_preview.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// The stage sequence the preview runs, factored out of `scene_preview.cpp` (issue #167, P3). P2
// produced a scene snapshot; P3 needs the *same laid-out tree* twice more - once recorded as a draw
// list and once painted through an RHI device - and the whole point of these gates is that the
// three consumers see one pipeline, not three copies of it. So the sequence lives here, in the
// CLI's order, and every consumer walks the value it returns.
//
// Deliberately no device, no filesystem, no clock: this is the part that runs natively and in Wasm
// unchanged. Choosing a backend is the caller's job.
namespace tessera::compiler::codegen::cli {

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
    // The preview's own stylesheet (`preview_stylesheet()`), the sheet the layout and the paint
    // pass both used. Kept so a painter resolves the same computed styles the layout did. Optional
    // because `CompiledStyleSheet` has no default constructor: empty exactly when `error` is
    // non-empty.
    std::optional<tessera::ui::style::CompiledStyleSheet> sheet;
    // Scene nodes whose style was resolved, and nodes `apply_layout` wrote a box to.
    std::size_t nodes = 0;
    std::size_t applied = 0;
};

// Runs parse -> analyse -> lower -> assign identity -> pick component -> evaluate state -> build
// the scene tree -> resolve computed style -> Yoga layout -> apply bounds for `text`, exactly as
// `scene_preview_document()` does (they share this function, so they cannot drift).
[[nodiscard]] PreparedScene prepare_scene(std::string_view text, const ScenePreviewOptions& options,
                                          std::string_view component = {});

// The preview's default stylesheet: the intrinsic element kinds get a size instead of collapsing to
// zero. Exposed because a consumer that re-runs style resolution (the draw list, the painter) must
// use the sheet the layout used.
[[nodiscard]] std::string_view preview_stylesheet() noexcept;

} // namespace tessera::compiler::codegen::cli
