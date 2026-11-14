// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/animation/runtime.hpp"
#include "tessera/ui/animation/transition.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/keyframes.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace tessera::ui::animation {

// Computes one node's style for the walk below. The frame pipeline binds its
// `paint::StyleProvider`; tests pass a stub. Mirrors `style::InteractionStyleFn` so
// `tessera_animation` stays free of a paint dependency. Never null.
using StyleBridgeFn = style::ComputedStyle (*)(void* context, const Node& node);

struct StyleBridgeResult {
    std::size_t nodes = 0;               // rendered nodes whose style was resolved
    std::size_t transitions_started = 0; // transition entries started by this sync
    std::size_t animations_started = 0;  // animation entries started by this sync
    std::size_t animations_stopped = 0;  // animation entries removed by this sync
    std::vector<AnimationId> ids;        // every id started, transitions then animations
};

// Computed style -> CSS transitions and animations, per node, once per style stage (issue #271).
//
// This is the missing production caller of `start_style_transitions`, and the frame-pipeline half
// of `sync_style_animations`: before it, a stylesheet could declare `transition: opacity 200ms
// linear` or `animation: fade 1s` and nothing ran, because only tests drove the runtime.
//
// Transitions need a "before" style, so the bridge keeps one snapshot per node that declares a
// transition. Snapshots exist only for those nodes, which keeps a large static tree from paying a
// full `ComputedStyle` per node; the trade-off is recorded in animation-runtime.md section 9: a
// transition declaration that appears in the *same* style change as the property change has no
// baseline and therefore starts on the next change instead of that one.
//
// The sync is change-driven, one walk per style stage, so it never runs on a paint-only frame.
class StyleBridge {
public:
    // Walks `root`, resolving each rendered node's style once. `keyframes` is the compiled sheet's
    // `@keyframes` table and may be empty (a sheet without animations), in which case the animation
    // half is a no-op. `on_complete` is the runtime's completion callback for started transitions.
    StyleBridgeResult sync(AnimationRuntime& runtime, const Node& root, StyleBridgeFn compute,
                           void* context, const std::vector<style::KeyframeBlock>& keyframes,
                           AnimationCallback on_complete = {});

    // Drops every snapshot: the next sync re-establishes the baseline and starts nothing.
    void clear() {
        previous_.clear();
    }

    // Nodes with a retained style snapshot (diagnostics and tests).
    [[nodiscard]] std::size_t tracked() const {
        return previous_.size();
    }

private:
    struct TrackedStyle {
        style::ComputedStyle style;
        std::uint64_t generation = 0;
    };

    // Returns whether a style snapshot was retained for `node`.
    void sync_node(AnimationRuntime& runtime, const Node& node, StyleBridgeFn compute,
                   void* context, const std::vector<style::KeyframeBlock>& keyframes,
                   const AnimationCallback& on_complete, std::uint64_t generation,
                   StyleBridgeResult& result);

    std::unordered_map<NodeId, TrackedStyle> previous_;
    // Bumped per sync so entries for nodes that left the rendered tree can be erased afterwards
    // without a second lookup.
    std::uint64_t generation_ = 0;
};

} // namespace tessera::ui::animation
