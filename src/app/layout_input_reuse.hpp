// SPDX-License-Identifier: MIT
//
// Which subtrees of the retained layout input a pass may keep (issue #735 follow-up, object-size
// rules in docs/guides/code-style.md, "Where the layout-input phase goes").
//
// Building the input rewrites one `LayoutInput` per node every pass, and at 12k nodes the walk
// through each node's `PaintData` is most of it. A subtree whose nodes carry no style, layout or
// paint dirt, and that no restyle reached, is exactly what the last pass wrote, so the pass skips
// it and takes its signature from `LayoutInput::subtree_signature`.
//
// "Exactly what the last pass wrote" needs more than the dirty flags, because a flag is consumed by
// `end_frame`:
//   * a frame that left the input unbuilt while dirt existed (the layout-unchanged gate) leaves
//     `stale` set, and the next pass rebuilds everything;
//   * a restyle the flags cannot attribute to one node (`!incremental`, a sheet that reads
//     siblings or `:has()`), a changed scene shape, or changed safe-area insets forces a full
//     rebuild;
//   * a restyle root rebuilds its whole subtree (descendants inherit), and the ancestors on the way
//     to it are visited, nothing more.

#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_set>

namespace tessera::ui {
class Node;
}

namespace tessera::app {

struct LayoutInputReuse {
    // More restyle roots than this and a full rebuild is cheaper than tracking them.
    static constexpr std::size_t kMaxRoots = 64;

    // The retained input may disagree with the scene in a way the dirty flags no longer show.
    bool stale = true;
    // A pass built the input since the last `end_frame`.
    bool built_this_frame = false;
    // The scene shape the input was built for (`scene_structure_epoch`).
    std::uint64_t structure_epoch = 0;

    // Per pass. `force_all` rebuilds every node. Otherwise `roots` are the restyled subtrees to
    // rebuild whole and `path` holds their ancestors, which are visited so the pass reaches them.
    // `path` is closed upward (a member's parent is a member), so a pass only asks about the
    // children of a node that is itself on the path.
    bool force_all = true;
    std::unordered_set<const ui::Node*> roots;
    std::unordered_set<const ui::Node*> path;
};

} // namespace tessera::app
