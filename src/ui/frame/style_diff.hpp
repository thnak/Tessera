// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

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
class DiffingStyleProvider final : public paint::StyleProvider {
public:
    DiffingStyleProvider(paint::StyleProvider& inner,
                         std::unordered_map<NodeId, style::ComputedStyle>& previous)
        : inner_(inner),
          previous_(previous) {}

    [[nodiscard]] style::ComputedStyle compute(const Node& node) override {
        style::ComputedStyle next = inner_.compute(node);
        seen_.insert(node.id());
        const auto found = previous_.find(node.id());
        if (found == previous_.end()) {
            previous_.emplace(node.id(), next);
        } else if (found->second.properties() != next.properties()) {
            // `opacity` composites the whole subtree (paint-pass.md section 14.1).
            const auto* before = found->second.find("opacity");
            const auto* after = next.find("opacity");
            const bool opacity_changed = (before == nullptr) != (after == nullptr) ||
                                         (before != nullptr && *before != *after);
            changed_.emplace_back(node.id(), opacity_changed);
            found->second = next;
        }
        return next;
    }

    // Forgets nodes this pass did not visit (removed or hidden), so the baseline cannot grow
    // without bound under churn.
    void prune() {
        for (auto it = previous_.begin(); it != previous_.end();) {
            it = seen_.contains(it->first) ? std::next(it) : previous_.erase(it);
        }
    }

    [[nodiscard]] const std::vector<std::pair<NodeId, bool>>& changed() const {
        return changed_;
    }

private:
    paint::StyleProvider& inner_;
    std::unordered_map<NodeId, style::ComputedStyle>& previous_;
    std::unordered_set<NodeId> seen_;
    std::vector<std::pair<NodeId, bool>> changed_;
};

} // namespace tessera::ui::frame
