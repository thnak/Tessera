// SPDX-License-Identifier: MIT
//
// Resolving the node ids one frame stage actually has, without building a map of the whole tree
// (issue #735, slice H).
//
// The frame pipeline used to build a `std::unordered_map<NodeId, Node*>` over every node of the
// scene on each changed frame, only to answer a handful of ids: the restyled nodes, the animation
// samples, the spring targets, the interpolated boxes. That was one allocation per tree node plus a
// hash insert per node, on a path that runs every frame. This resolves the k ids a stage has with
// one pre-order walk that binary-searches each node's id, so the cost is O(n log k) and the only
// storage is the k-entry buffer the caller retains between frames.
//
// Header-only and beside `redraw_cull.hpp` for the same reason: it is one small type with no state
// of its own, and `frame_pipeline.cpp` is at its line limit.

#pragma once

#include "tessera/ui/animation/runtime.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/node_fwd.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace tessera::ui::frame::detail {

// One id to resolve and the node it resolved to. The buffer is a plain vector of these so the
// pipeline can own it without naming a type from this private header in its public one.
using NodeLookupBuffer = std::vector<std::pair<NodeId, Node*>>;

// A view over a retained `NodeLookupBuffer`. `reset`, `add` each id, `build` once against the tree,
// then `find` each id. `build` sorts the buffer by id and one pre-order walk fills in the node for
// every entry whose id it meets, so `find` is a binary search. A duplicate id resolves to the last
// node in pre-order, which is the node the map this replaced kept. The buffer keeps its capacity
// across frames, so a steady frame allocates nothing.
class NodeLookup {
public:
    explicit NodeLookup(NodeLookupBuffer& entries)
        : entries_(&entries) {}

    void reset() {
        entries_->clear();
    }
    void add(NodeId id) {
        entries_->emplace_back(id, nullptr);
    }

    // Sorts the collected ids and resolves them against `root` in one walk. An id the tree does not
    // contain keeps a null node, so a caller skips it instead of dereferencing.
    void build(Node& root) {
        if (entries_->empty()) {
            return;
        }
        std::sort(entries_->begin(), entries_->end(), by_id);
        resolve_in(root);
    }

    [[nodiscard]] Node* find(NodeId id) const {
        const auto found =
            std::lower_bound(entries_->begin(), entries_->end(), id,
                             [](const auto& entry, NodeId key) { return entry.first < key; });
        return found != entries_->end() && found->first == id ? found->second : nullptr;
    }

private:
    static bool by_id(const std::pair<NodeId, Node*>& lhs, const std::pair<NodeId, Node*>& rhs) {
        return lhs.first < rhs.first;
    }

    void resolve_in(Node& node) {
        const auto first =
            std::lower_bound(entries_->begin(), entries_->end(), node.id(),
                             [](const auto& entry, NodeId key) { return entry.first < key; });
        for (auto entry = first; entry != entries_->end() && entry->first == node.id(); ++entry) {
            entry->second = &node;
        }
        for (const auto& child : node.children()) {
            resolve_in(*child);
        }
    }

    NodeLookupBuffer* entries_;
};

// Fills `lookup` with `project(item)` for every item of `items` and resolves them against `root`.
// The one entry point every frame stage uses, so the "reset, add, build" order cannot be got wrong
// at a call site.
template<typename Range, typename Projection>
void resolve_ids(Node& root, NodeLookup& lookup, const Range& items, Projection project) {
    lookup.reset();
    for (const auto& item : items) {
        lookup.add(project(item));
    }
    lookup.build(root);
}

// The animation half of a frame: the samples of one tick name a handful of nodes, so resolving them
// is one walk of the tree and one binary search per node, never a map build.
inline void resolve_samples(Node& root, NodeLookup& lookup,
                            const std::vector<animation::AnimationSample>& samples) {
    resolve_ids(root, lookup, samples,
                [](const animation::AnimationSample& sample) { return sample.target; });
}

} // namespace tessera::ui::frame::detail
