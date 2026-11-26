// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/eval/evaluator.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/ir/source_span.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// IR -> live scene tree for the in-memory preview (issue #166). The evaluator (src/compiler/eval)
// supplies expression values; this file applies them to the same `ui::Node` surface generated C++
// uses (`set_element_kind`, `set_automation_id`, `set_inline_style`, `set_paint_data`,
// `add_child`), so a preview tree is the tree a native build would have built for the same IR, not
// a second model of it. Style and layout are separate steps (`scene_layout.hpp`), because they
// operate on the tree this produces and can be re-run without rebuilding it.
//
// Not covered by this slice, and reported as an unsupported construct instead of guessed at:
// `CustomElement` nodes and virtual collections. A user component instance is built (issue #594)
// when the caller passes the module that holds its callee. The corpus in
// `scripts/ci/scene_snapshot_parity.py` stays inside what is implemented; the omission is recorded
// in docs/design/compiler-wasm.md section 7.1.
namespace tessera::compiler::codegen::cli {

// Scene-side limits, matching the harness's snapshot bounds so a preview tree can never be larger
// than the document that reports it.
inline constexpr std::size_t kMaxPreviewNodes = 4096;
inline constexpr std::size_t kMaxPreviewDepth = 32;

struct SceneBuildResult {
    // Non-empty when the tree could not be built. The caller discards the tree: a partially built
    // tree would be a snapshot of a program that does not exist.
    std::string error;
    std::size_t nodes = 0;
    // One entry per built scene node that declares an IR event, in build order: the scene node's
    // automation id (`uiId`) and the IR handler id per event name. The scene graph stores a
    // type-erased C callback (`Node::bind_event`), so the mapping from a dispatched scene event
    // back to the IR handler it must run is the session's to install; this is the data it needs and
    // nothing more. Empty for a tree whose nodes declare no events.
    struct SceneEventHandler {
        std::uint64_t automation_id = 0;
        std::string event;   // the IR/schema event name (`onClick`, `onKeyDown`, ...)
        std::string handler; // the IR handler id: `handler::N` or `bind::name`
    };
    std::vector<SceneEventHandler> handlers;
    // The IR source span of every built scene node, keyed by the scene node id the walk assigned
    // (issue #171's `source.patch` slice). This is a SIDE TABLE rather than a `ui::Node` field on
    // purpose: `ui::Node` is a public header with an enforced `kNodeSizeBudget` and a
    // `static_assert`, and a span is a design-time fact that only the commit path reads. Growing
    // the node would cost every screen memory for a value no frame touches.
    //
    // Recorded here because this walk is the last place that still holds the `ir::Node`; after it
    // returns, the scene node is all that is left and it carries no span.
    struct SceneNodeSource {
        std::uint64_t node_id = 0;
        ir::SourceSpan span;
    };
    std::vector<SceneNodeSource> sources;
    // One entry per IR `ref` that resolved to a built scene node, in build order: the `ref::` id
    // and the scene node it names. A behavior attaches to the nodes its target refs resolved to
    // (behavior-hooks-v1.md section 8), so this is the table the preview session needs to build the
    // same `Targets` a generated base would. A ref whose target node was not built (a conditional
    // branch that did not take, a list that is empty) records nothing, and a behavior whose targets
    // are not all present attaches nothing -- the same rule the generated base follows.
    struct SceneRefTarget {
        std::string ref; // the IR `ref::` id
        std::uint64_t node_id = 0;
    };
    std::vector<SceneRefTarget> ref_targets;
};

// Builds the tree of `component` under `root`. `root` is the window content node: it is given the
// viewport size as inline style (the way `AppHost` sizes it) and the component's `root` node
// becomes its child, exactly as a generated base's `build()` attaches its tree to the parent it is
// given.
//
// State and animated cells are evaluated from their initializers into `scope` first, so every node
// expression reads the values the component starts with. The same `scope` is what a later action
// would mutate, which is what makes this the evaluator's path rather than a static reading of the
// IR.
//
// **Lifetime:** `ui::Node` borrows its element metadata strings (`set_element_kind`,
// `set_element_id`, `set_class_name`, `set_role`, `set_aria`, `set_inline_style`), and the ones
// this walk applies point into `component`'s strings. The IR module must therefore outlive the
// scene, as it does in every caller (a generated base's literals are static for the same reason).
//
// `module` is where a user component instance (`NodeKind::Component`) finds its callee. Without it
// an instance is reported as unsupported, which is what a caller that has no project gets.
[[nodiscard]] SceneBuildResult build_scene(const ir::Component& component, ui::Node& root,
                                           eval::Scope& scope,
                                           const ir::IrModule* module = nullptr);

} // namespace tessera::compiler::codegen::cli
