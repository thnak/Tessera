// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/animation_style.hpp"
#include "tessera/ui/animation/keyframes.hpp"
#include "tessera/ui/animation/style_bridge.hpp"
#include "tessera/ui/animation/transition_runtime.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::animation {
namespace {

// The value a running transition on `property` currently displays, or an empty string when nothing
// is running for it. This is the "from" side of an interrupted transition: retargeting a property
// mid-flight continues from what is on screen instead of snapping back to the value the previous
// transition started from (animation-runtime.md section 9).
std::string displayed_value(const AnimationRuntime& runtime, std::uint64_t target,
                            std::string_view property) {
    for (const AnimationEntry& entry : runtime.entries()) {
        if (entry.target != target || entry.property != property) {
            continue;
        }
        const std::vector<style::Declaration> sampled =
            sample_keyframes(entry.keyframes, entry.timing, entry.elapsed);
        for (const style::Declaration& declaration : sampled) {
            if (declaration.property == property) {
                return declaration.value;
            }
        }
    }
    return {};
}

// True when the declaration list actually animates something. A zero duration completes on the same
// tick, so the new computed value already applies and starting an entry would only leave a settled
// fill behind; CSS gives a zero-duration transition no run either.
bool has_duration(const std::vector<TransitionSpec>& transitions) {
    return std::any_of(transitions.begin(), transitions.end(),
                       [](const TransitionSpec& spec) { return spec.duration > 0.0f; });
}

} // namespace

void StyleBridge::sync_node(AnimationRuntime& runtime, const Node& node, StyleBridgeFn compute,
                            void* context, const std::vector<style::KeyframeBlock>& keyframes,
                            const AnimationCallback& on_complete, std::uint64_t generation,
                            StyleBridgeResult& result) {
    if (!node.rendered()) {
        // A node outside the rendered tree is not animated: leaving or entering it re-establishes
        // the baseline rather than animating the change, which is what CSS does for `display: none`
        // and matches the paint / hit-test / a11y gate.
        return;
    }

    style::ComputedStyle current = compute(context, node);
    ++result.nodes;

    const TransitionParseResult parsed = parse_style_transitions(current);
    const bool transitions = parsed.ok && has_duration(parsed.transitions);

    const auto found = previous_.find(node.id());
    if (found == previous_.end()) {
        // First observed style for this node. It is the baseline, not a change, so nothing starts:
        // the initial style never transitions. A snapshot is retained only for a node that declares
        // a transition, so a large static tree does not pay a full `ComputedStyle` per node.
        if (transitions) {
            previous_.emplace(node.id(), TrackedStyle{std::move(current), generation});
        }
    } else {
        if (transitions) {
            // The "from" side: a property with a running transition continues from the value it is
            // displaying; every other property uses its previous computed value. The stored
            // snapshot stays the computed style, so the next diff still compares computed values.
            style::ComputedStyle before = found->second.style;
            for (const TransitionSpec& spec : parsed.transitions) {
                const std::string shown = displayed_value(runtime, node.id(), spec.property);
                if (!shown.empty()) {
                    before.set(spec.property, shown);
                }
            }
            const std::vector<AnimationId> ids = start_style_transitions(
                runtime, node.id(), parsed.transitions, before, current, on_complete);
            result.transitions_started += ids.size();
            result.ids.insert(result.ids.end(), ids.begin(), ids.end());
        }
        if (transitions) {
            found->second.style = std::move(current);
            found->second.generation = generation;
        } else {
            // The transition declaration went away; drop the snapshot so the node stops being
            // tracked until it declares one again.
            previous_.erase(found);
        }
    }

    // The animation half of the bridge. `sync_style_animations` is idempotent, so it runs for every
    // rendered node on every style stage and stops the names the current style no longer declares.
    // `animation-name` has an empty registry initial value, so a node without a declaration parses
    // to an empty desired list and this is a no-op in the common case.
    if (!keyframes.empty()) {
        const StyleAnimationSync sync =
            sync_style_animations(runtime, node.id(), current, keyframes);
        result.animations_started += sync.started;
        result.animations_stopped += sync.stopped;
        result.ids.insert(result.ids.end(), sync.ids.begin(), sync.ids.end());
    }

    for (const auto& child : node.children()) {
        sync_node(runtime, *child, compute, context, keyframes, on_complete, generation, result);
    }
}

StyleBridgeResult StyleBridge::sync(AnimationRuntime& runtime, const Node& root,
                                    StyleBridgeFn compute, void* context,
                                    const std::vector<style::KeyframeBlock>& keyframes,
                                    AnimationCallback on_complete) {
    StyleBridgeResult result;
    if (compute == nullptr) {
        return result;
    }
    // Each snapshot touched by this walk is stamped with the sync's generation, so a node that left
    // the rendered tree (removed, `display: none`, `visibility: hidden`) is dropped afterwards
    // instead of being diffed against a stale style when it comes back.
    const std::uint64_t generation = ++generation_;
    sync_node(runtime, root, compute, context, keyframes, on_complete, generation, result);
    for (auto it = previous_.begin(); it != previous_.end();) {
        if (it->second.generation != generation) {
            it = previous_.erase(it);
        } else {
            ++it;
        }
    }
    return result;
}

} // namespace tessera::ui::animation
