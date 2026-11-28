// SPDX-License-Identifier: MIT

#pragma once

#include "scene_build.hpp"
#include "tessera/compiler/eval/evaluator.hpp"
#include "tessera/compiler/ir/component.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/ir/node.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// Fills `scope` with a component's declared cells (state, animated values, environment, behavior
// outputs) from their initializers. A cell already present is kept (a value carried across an
// edit).
[[nodiscard]] bool seed_component_scope(const ir::Component& component, eval::Scope& scope,
                                        std::string& error);

class Builder final {
public:
    Builder(ui::Node& root, eval::Scope& scope, SceneBuildResult& result,
            const ir::IrModule* module = nullptr, InstanceScopes* instances = nullptr)
        : root_(root),
          scope_(&scope),
          result_(result),
          module_(module),
          instances_(instances) {}

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

    // The component's `ref` declarations, so the walk can record which scene node each one resolved
    // to (a behavior's target). Set once by `build()`.
    // The pending-UI session that decides which subtree each `Suspense` boundary shows, and that
    // the evaluator reads `res::` values from. Null: boundaries show their children.
    void set_pending(eval::PendingSession* pending) noexcept {
        pending_ = pending;
        evaluator_.set_pending_session(pending);
    }

    void set_component_refs(const std::vector<ir::Ref>* refs) noexcept {
        component_refs_ = refs;
    }

private:
    bool build_conditional_child(const ir::Node& child, ui::Node& parent, std::size_t depth);
    bool build_repeated_child(const ir::Node& child, ui::Node& parent, std::size_t depth);
    // A user component instance (ADR in docs/design/preview-component-instances.md): props are
    // evaluated in the caller's scope into a fresh callee scope, the callee's root is built under
    // the instance's host node, and a `Slot` splices the instance's children back in under the
    // caller's scope.
    bool build_instance(const ir::Node& node, ui::Node& host, std::size_t depth);
    // A `Suspense` boundary: its live subtree, spliced into `parent` (no scene node of its own).
    bool build_suspense(const ir::Node& node, ui::Node& parent, std::size_t depth);
    bool build_slot(const ir::Node& slot, ui::Node& parent, std::size_t depth);

    // Everything an instance replaces while its callee builds, restored when it is done.
    struct Context {
        eval::Scope* scope = nullptr;
        int loop_depth = 0;
        const std::vector<ir::Ref>* refs = nullptr;
        std::uint64_t salt = 0;
        std::string key;       // the instance path ("" in the root component)
        std::size_t frame = 0; // index into `result_.frames`
        std::string loop_path; // loop item indices on the way down, so repeated instances differ
    };
    struct CallerFrame {
        Context context;
        const ir::Node* instance = nullptr;
    };
    [[nodiscard]] Context capture() const;
    void apply(const Context& context);

    // A metadata string the node supplies: its literal, or the value of a dynamic expression
    // (kept alive in `result_.strings`). False with `result_.error` set when evaluation failed.
    bool metadata_string(const ir::Node& node, std::string_view name, std::string_view& out,
                         bool& present);

    ui::Node& root_;
    eval::Scope* scope_;
    SceneBuildResult& result_;
    const ir::IrModule* module_ = nullptr;
    // The instances being built, innermost last: each entry is the caller's state to return to.
    std::vector<CallerFrame> frames_;
    InstanceScopes* instances_ = nullptr;
    // Non-zero inside a callee: a hash of the instance path, mixed into automation ids so two
    // instances of one component do not share the ids of its nodes (and the ids survive a rebuild).
    std::uint64_t salt_ = 0;
    std::string instance_key_;
    std::size_t frame_index_ = 0;
    std::string loop_path_;
    eval::Evaluator evaluator_;
    eval::PendingSession* pending_ = nullptr;
    std::uint64_t next_node_id_ = 2; // 1 is the window content root the caller owns
    int loop_depth_ = 0;
    // The component's `ref` declarations, borrowed from the IR for the duration of one build.
    const std::vector<ir::Ref>* component_refs_ = nullptr;
};

} // namespace tessera::compiler::codegen::cli::internal
