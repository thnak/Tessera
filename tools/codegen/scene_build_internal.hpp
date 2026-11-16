// SPDX-License-Identifier: MIT

#pragma once

#include "scene_build.hpp"
#include "tessera/compiler/eval/evaluator.hpp"
#include "tessera/compiler/ir/component.hpp"
#include "tessera/compiler/ir/node.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

// The IR -> scene walk behind `build_scene()`, split across two translation units so each one stays
// inside the file-size limit (issue #389 pushed the walk past it):
//
//   * `scene_build_walk.cpp`     one node: creation, identity, metadata, focus/scroll, its text;
//   * `scene_build_children.cpp` a node's children: the static order, a conditional branch, and a
//                                repeated child's items with their per-item loop locals.
//
// The split is mechanical. What the preview *decides* about the two dynamic forms -- which state a
// condition reads, what supplies a list's items -- lives in `scene_build_dynamic.hpp`, so that
// decision is readable in one place instead of being spread over two files.
namespace tessera::compiler::codegen::cli::internal {

class Builder final {
public:
    Builder(ui::Node& root, eval::Scope& scope, SceneBuildResult& result)
        : root_(root),
          scope_(scope),
          result_(result) {}

    // Builds `component`'s tree under `root_` (the window content node the caller owns).
    [[nodiscard]] bool build(const ir::Component& component);

    // Builds `node` and its subtree as one child of `parent`. Pre-order: the node is attached
    // before its children are built, and node ids are handed out in that same order, so the same
    // source and state always produce the same ids.
    [[nodiscard]] bool build_node(const ir::Node& node, ui::Node& parent, std::size_t depth);

    // One node's children in source order (scene_build_children.cpp).
    [[nodiscard]] bool build_children(const ir::Node& node, ui::Node& parent, std::size_t depth);

    [[nodiscard]] bool fail(std::string message) {
        result_.error = std::move(message);
        return false;
    }
    [[nodiscard]] bool unsupported(const std::string& what) {
        return fail("TESSERA_SCENE_UNSUPPORTED: " + what);
    }

    // The loop-local names currently in scope, so a nested template is reported the way codegen
    // reports it rather than silently repeating over the wrong list.
    [[nodiscard]] int loop_depth() const noexcept {
        return loop_depth_;
    }
    void set_loop_depth(int depth) noexcept {
        loop_depth_ = depth;
    }

private:
    bool build_conditional_child(const ir::Node& child, ui::Node& parent, std::size_t depth);
    bool build_repeated_child(const ir::Node& child, ui::Node& parent, std::size_t depth);

    ui::Node& root_;
    eval::Scope& scope_;
    SceneBuildResult& result_;
    eval::Evaluator evaluator_;
    std::uint64_t next_node_id_ = 2; // 1 is the window content root the caller owns
    int loop_depth_ = 0;
};

} // namespace tessera::compiler::codegen::cli::internal
