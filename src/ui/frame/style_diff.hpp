// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/node_fwd.hpp"

#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tessera::ui::frame {

// Records, per node, whether a recompute differs from the last style this pipeline saw. A change
// to any computed property is a visible input change (dirty-rendering.md section 4.1: never show a
// stale frame), so the node needs a repaint even when no layout or scene-consumed property moved.
// The first sighting of a node is not a change: a new node dirties itself.
//
// Both containers are **caller-owned and retained across frames** (issue #701). They used to be
// members of this object, which the pipeline constructs per sync call, so `seen_` rehashed to one
// entry per node on every restyle: 601 nodes x 120 frames = 72,120 allocations, 89% of a
// layout-dirty frame's total. Passing them in keeps the capacity, so a steady frame allocates
// nothing here.
class DiffingStyleProvider final : public paint::StyleProvider {
public:
    // Declared at namespace scope in `frame_pipeline.hpp` so the retained map can name it.
    using Baseline = StyleBaseline;

    DiffingStyleProvider(paint::StyleProvider& inner,
                         std::unordered_map<NodeId, Baseline>& previous, std::uint64_t generation,
                         std::vector<std::pair<NodeId, bool>>& changed)
        : inner_(inner),
          previous_(previous),
          generation_(generation),
          changed_(changed) {
        changed_.clear();
    }

    [[nodiscard]] style::ComputedStyle compute(const Node& node) override {
        return resolve(node);
    }

    // Steady state costs one pointer comparison and no copy (issue #701). The baseline stores the
    // style's *identity* -- its shared entries block -- plus the one value the diff needs to read
    // from the previous style, `opacity`.
    //
    // Why not a stored copy compared with `equals`: `SceneStyleTree::begin_memo` drops the memo on
    // a non-incremental restyle, so every node's style is recomputed into a fresh entries block. A
    // stored copy therefore had to be re-copied on every such frame (601 nodes x 120 frames =
    // 72,120 allocations, 89% of a layout-dirty frame). The identity is a `shared_ptr`, so storing
    // it is a refcount bump and comparing it is a pointer test; `opacity` is a short CSS number, so
    // it lives in the string's small-buffer form and allocates nothing.
    //
    // The trade: a recompute that produces an *equal* style in a *new* block now reads as a change,
    // so the node is marked paint-dirty and repainted. That is the conservative direction -- a
    // spurious repaint, never a stale frame -- and the paint cache makes the extra repaint cheap.
    // A restyle that keeps its entries (the incremental path) still hits the pointer test.
    [[nodiscard]] const style::ComputedStyle& resolve(const Node& node) override {
        const style::ComputedStyle& next = inner_.resolve(node);
        const std::string* const next_opacity = next.find("opacity");
        const auto found = previous_.find(node.id());
        if (found == previous_.end()) {
            previous_.emplace(
                node.id(), Baseline{next.identity(), next, generation_, next_opacity != nullptr});
            return next;
        }
        found->second.generation = generation_;
        if (next.has_identity(found->second.identity)) {
            return next; // same entries block: the effective style cannot differ
        }
        // A new entries block. The values may still be identical (a recompute into a fresh block),
        // and an identical computed style must plan no redraw, so compare before calling it a
        // change. `equals` compares the deltas, not the ~100-entry merged view.
        if (!found->second.style.equals(next)) {
            // `opacity` composites the whole subtree (paint-pass.md section 14.1).
            const bool opacity_present = next_opacity != nullptr;
            changed_.emplace_back(node.id(), opacity_present && found->second.opacity_present);
            found->second.opacity_present = opacity_present;
        }
        // Adopt the new block and its values, so the next frame's comparison is the pointer test.
        found->second.identity = next.identity();
        found->second.style = next;
        return next;
    }

    // Forgets nodes this pass did not visit (removed or hidden), so the baseline cannot grow
    // without bound under churn. A node the pass visited carries this sync's generation, so the
    // test is a stamp comparison instead of a set lookup (issue #701).
    void prune() {
        for (auto it = previous_.begin(); it != previous_.end();) {
            it = it->second.generation == generation_ ? std::next(it) : previous_.erase(it);
        }
    }

    [[nodiscard]] const std::vector<std::pair<NodeId, bool>>& changed() const {
        return changed_;
    }

private:
    paint::StyleProvider& inner_;
    std::unordered_map<NodeId, Baseline>& previous_;
    std::uint64_t generation_ = 0;
    std::vector<std::pair<NodeId, bool>>& changed_;
};

} // namespace tessera::ui::frame
