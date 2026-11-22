// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/automation/snapshot.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

namespace tessera::ui::automation {

// Snapshot bounds for one live scene tree. They exist so a caller cannot make the process copy an
// unbounded tree: the node cap bounds the builder's allocations, the depth cap bounds the recursion
// (and the structural path strings), and the path cap keeps each emitted path small.
inline constexpr std::size_t kMaxSnapshotNodes = 65536;
inline constexpr std::size_t kMaxSnapshotDepth = 64;
inline constexpr std::size_t kMaxSnapshotPathBytes = 256;

// Source-level identity for one node, keyed by its automation id: the caller's structural path and
// component id (a generated manifest's, for the harness). A `ui::Node` carries neither, so the walk
// asks for them. This seam is a callback rather than the automation lane's `NodeAnnotations` map so
// this header stays free of that module: the walk is scene + snapshot only.
struct SceneAnnotation {
    std::string_view path; // empty keeps the path the walk derives from the live tree
    std::uint64_t component_id = 0;
};

// Returns true when it filled `out` for `automation_id`; false leaves the walk's own path and a
// component id of 0. Never null unless the caller has no annotations at all.
using SceneAnnotationFn = bool (*)(void* context, std::uint64_t automation_id,
                                   SceneAnnotation& out);

// The scene -> snapshot walk of docs/specs/automation-ids-v1.md section 5.2: it reads the live
// `ui::Node` tree (automation id, element kind, accessible name, text payload, world bounds,
// visible/enabled/focused/selected state, list item key) into the plain `SnapshotSourceNode` tree
// that `build_snapshot()` turns into the canonical `UiSnapshot`.
//
// It lives in its own small target rather than in the harness because there are two callers that
// must agree byte for byte: the native headless harness (`src/harness/engine_host_snapshot.cpp`)
// and the design-time preview's in-memory evaluate -> layout -> snapshot path
// (`tools/codegen/scene_preview.cpp`, issue #166). The walk needs no filesystem, no GPU, no RTTI
// and no clock, so both lanes and the Wasm module run this same code; a second copy would be a
// second definition of the artifact the parity gate exists to protect.
struct SceneSnapshotOptions {
    // `sequence` is written into the header as-is; `include_hidden` emits a hidden node with
    // `visible=false` instead of dropping it and its subtree.
    SnapshotOptions snapshot{};
    std::size_t max_nodes = kMaxSnapshotNodes;
    std::size_t max_depth = kMaxSnapshotDepth;
    // Optional source-level identity (the harness binds a generated manifest here).
    SceneAnnotationFn annotate = nullptr;
    void* annotation_context = nullptr;
};

// Why the walk stopped. A capped tree is a hard failure rather than a truncated snapshot: a partial
// document would compare equal to a partial document from the other side and pass a parity gate it
// should fail.
enum class SceneSnapshotErrorCode : std::uint8_t {
    TooDeep,      // the tree nests deeper than `max_depth`
    TooManyNodes, // the tree has more nodes than `max_nodes`
};

struct SceneSnapshotError {
    SceneSnapshotErrorCode code = SceneSnapshotErrorCode::TooManyNodes;
    std::size_t limit = 0;  // the cap that was exceeded
    std::size_t actual = 0; // the depth or count that exceeded it
};

// Walks `root` in preorder (document order). The root's path is `root`; a child is
// `<parent path>/<Kind>[<index>]`, capped at kMaxSnapshotPathBytes, so two walks of the same tree
// produce the same paths. Deterministic: the same tree and options always produce the same
// snapshot, and it never allocates per node beyond the snapshot's own entries.
[[nodiscard]] std::expected<UiSnapshot, SceneSnapshotError>
snapshot_scene(const Node& root, const SceneSnapshotOptions& options = {});

} // namespace tessera::ui::automation
