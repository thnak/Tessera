// SPDX-License-Identifier: MIT
//
// The style-to-scene sync half of `FramePipeline` (issue #735, slice B). Split out of
// `frame_pipeline.cpp`, which is at its size limit: the sync is one responsibility (apply computed
// style to the scene, then start the transitions and animations it declares) and the rest of the
// file is the tick, damage and paint halves.
//
// `sync_style` walks the whole tree. `sync_style_scoped` visits only the restyled subtrees and the
// ancestors that refold `glass_subtree`, and is sound only when the caller can prove every other
// node kept its style and its box - which is exactly what the layout-unchanged gate proves.

#include "node_lookup.hpp"
#include "style_diff.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/animation/loop_bridge.hpp"
#include "tessera/ui/animation/property_dirty.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/style/interaction_style.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace tessera::ui::frame {
namespace {

namespace rhi = tessera::rendering::rhi;

// Adapts the paint provider (compiled stylesheet, animation overlay or a test stub) to the
// interaction sync so the scene gets `pointer-events`/`user-select` from the same computed style
// paint and layout see.
const style::ComputedStyle& interaction_style_thunk(void* context, const Node& node) {
    return static_cast<paint::StyleProvider*>(context)->resolve(node);
}

// An empty `@keyframes` table for a sheet without animations or a pipeline with no keyframes
// installed. A namespace-scope object rather than a function-local static, so the style stage does
// not pay a thread-safe-init guard per frame.
const std::vector<style::KeyframeBlock> kNoKeyframes;

} // namespace

paint::SceneStyleSyncResult FramePipeline::sync_style_scoped(Node& root,
                                                             paint::StyleProvider& styles,
                                                             std::span<const Node* const> roots,
                                                             const FrameTick& tick,
                                                             bool force) const {
    if (!force && !tick.needs_style && !tick.needs_layout) {
        style_bridge_snapshot_ = {};
        return {};
    }
    DiffingStyleProvider diffing(styles, previous_styles_);
    paint::SceneStyleSyncResult result =
        paint::sync_scene_style_scoped(diffing, roots, config_.glass, &scene_sync_memo_);
    // No `prune()` here, deliberately. `prune()` drops every node this pass did not visit, and a
    // scoped pass visits only the restyled subtrees - so pruning would erase the diff baseline for
    // every unchanged node. The next change to one of those nodes would then read as a first
    // sighting, which `DiffingStyleProvider` correctly treats as "not a change", and the frame
    // would plan no redraw (a stale frame). The baseline for an unvisited node is still accurate
    // because the gate proved its style did not change; a node that left the tree keeps a stale
    // entry until the next unscoped sync prunes it, which is a bounded, harmless leak (a reused id
    // can only cause a spurious repaint, never a missed one).
    if (!diffing.changed().empty()) {
        // Resolve just the changed ids, not a map of the whole tree (issue #735, slice H).
        detail::NodeLookup lookup(node_lookup_buffer_);
        detail::resolve_ids(root, lookup, diffing.changed(),
                            [](const auto& entry) { return entry.first; });
        for (const auto& [id, opacity_changed] : diffing.changed()) {
            if (Node* node = lookup.find(id); node != nullptr) {
                if (opacity_changed) {
                    node->mark_subtree_dirty(Dirty::Paint | Dirty::Render);
                } else {
                    node->mark_dirty(Dirty::Paint | Dirty::Render);
                }
            }
        }
    }
    // Interaction state and the transition/animation bridge still walk the whole tree: both diff
    // against a per-node snapshot, so scoping them needs the same "unchanged" proof per node that
    // the scene sync gets from the gate. Kept whole here so this slice changes one walk, not three.
    sync_interaction_style(root, &interaction_style_thunk, &styles, &interaction_sync_memo_);
    style_bridge_snapshot_ =
        style_bridge_state_.sync(*animations_, root, &interaction_style_thunk, &styles,
                                 style_keyframes_ != nullptr ? *style_keyframes_ : kNoKeyframes);
    if (!style_bridge_snapshot_.ids.empty()) {
        loop_->request_frame();
    }
    return result;
}

paint::SceneStyleSyncResult FramePipeline::sync_style(Node& root, paint::StyleProvider& styles,
                                                      const FrameTick& tick, bool force) const {
    if (!force && !tick.needs_style && !tick.needs_layout) {
        style_bridge_snapshot_ = {};
        return {};
    }
    DiffingStyleProvider diffing(styles, previous_styles_);
    paint::SceneStyleSyncResult result =
        paint::sync_scene_style(root, diffing, config_.glass, &scene_sync_memo_);
    diffing.prune();
    if (!diffing.changed().empty()) {
        detail::NodeLookup lookup(node_lookup_buffer_);
        detail::resolve_ids(root, lookup, diffing.changed(),
                            [](const auto& entry) { return entry.first; });
        for (const auto& [id, opacity_changed] : diffing.changed()) {
            Node* const target = lookup.find(id);
            if (target == nullptr) {
                continue;
            }
            if (opacity_changed) {
                target->mark_subtree_dirty(Dirty::Paint | Dirty::Render);
            } else {
                target->mark_dirty(Dirty::Paint | Dirty::Render);
            }
        }
    }
    // Interaction state the scene consumes (`pointer-events`, `user-select`; stylesheet-v1 section
    // 11, issue #247), same seam and frame so the halves never disagree about a restyled node.
    sync_interaction_style(root, &interaction_style_thunk, &styles, &interaction_sync_memo_);
    // Transition and animation declarations become real here (issue #271): the diff against the
    // previous style starts the transitions the new style declares, and the animation sync
    // reconciles the running set against `animation-*`. Both feed the runtime the next tick
    // samples.
    style_bridge_snapshot_ =
        style_bridge_state_.sync(*animations_, root, &interaction_style_thunk, &styles,
                                 style_keyframes_ != nullptr ? *style_keyframes_ : kNoKeyframes);
    if (!style_bridge_snapshot_.ids.empty()) {
        // The transition or animation starts now, after this tick already decided whether the loop
        // may idle: ask for the next frame so the first sample is rendered.
        loop_->request_frame();
    }
    return result;
}

void FramePipeline::sample_started_animations(Node& root) {
    if (!interpolate_ || style_bridge_snapshot_.ids.empty()) {
        return;
    }
    const animation::AnimationTickResult first =
        animation::tick_animations(*animations_, *loop_, 0.0);
    detail::NodeLookup lookup(node_lookup_buffer_);
    detail::resolve_samples(root, lookup, first.samples);
    for (const animation::AnimationSample& sample : first.samples) {
        Node* const target = lookup.find(sample.target);
        if (target == nullptr) {
            continue;
        }
        capture_paint_sample(current_samples_, *target, sample.declarations);
        if (const auto captured = current_samples_.find(sample.target);
            captured != current_samples_.end() && !previous_samples_.contains(sample.target)) {
            previous_samples_[sample.target] = captured->second;
        }
        const Dirty dirty = animation::dirty_for_declarations(sample.declarations);
        if (dirty != Dirty::Clean) {
            target->mark_dirty(dirty);
        }
    }
}

} // namespace tessera::ui::frame