// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/damage_pass.hpp"
#include "tessera/ui/layout/pixel_snap.hpp"

#include <iterator>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace tessera::ui::layout {
namespace {

constexpr Dirty kDamageDomain = Dirty::Paint | Dirty::Render;

bool has_area(const rendering::rhi::Rect& bounds) {
    return bounds.width > 0.0f && bounds.height > 0.0f;
}

void collect_ids(const Node& node, std::unordered_set<NodeId>& ids) {
    ids.insert(node.id());
    for (const auto& child : node.children()) {
        collect_ids(*child, ids);
    }
}

} // namespace

DamageTracker::DamageTracker(std::size_t max_entries)
    : capacity_(max_entries) {
    if (capacity_ == 0) {
        throw std::invalid_argument("damage tracker capacity must be greater than zero");
    }
}

DamagePassResult DamageTracker::add_scene_damage(const DirtyPass& dirty,
                                                 rendering::DamageAccumulator& damage,
                                                 float dpi_scale, float margin) {
    DamagePassResult result;
    result.overflowed = dirty.overflowed();

    for (const DirtyNode& entry : dirty.nodes()) {
        if (entry.node == nullptr || !has_any(entry.flags, kDamageDomain)) {
            continue;
        }

        const rendering::rhi::Rect current = node_damage_rect(*entry.node);
        const auto found = index_.find(entry.node->id());
        if (found != index_.end()) {
            const rendering::rhi::Rect previous = found->second->bounds;
            if (has_area(previous)) {
                damage.add(snap_rect(previous, dpi_scale), margin);
            }
        }

        if (has_area(current)) {
            damage.add(snap_rect(current, dpi_scale), margin);
        }
        ++result.nodes;

        if (found != index_.end()) {
            found->second->bounds = current;
            entries_.splice(entries_.end(), entries_, found->second);
        } else {
            entries_.push_back(Entry{entry.node->id(), current});
            index_.emplace(entry.node->id(), std::prev(entries_.end()));
            if (entries_.size() > capacity_) {
                index_.erase(entries_.front().id);
                entries_.pop_front();
            }
        }
    }

    // A truncated dirty list cannot prove the damage is complete: the missing nodes may have
    // changed pixels anywhere in their (unknown) bounds. Over-damage is cheap and safe;
    // under-damage leaves stale pixels, so an overflow forces a full-target redraw.
    if (dirty.overflowed()) {
        damage.force_full_redraw();
    }
    result.rects = damage.rects().size();
    result.full_redraw = damage.full_redraw();
    return result;
}

std::size_t DamageTracker::prune_removed(const Node& root) {
    std::unordered_set<NodeId> alive;
    collect_ids(root, alive);

    std::size_t removed = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (alive.find(it->id) == alive.end()) {
            index_.erase(it->id);
            it = entries_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

void DamageTracker::clear() {
    entries_.clear();
    index_.clear();
}

std::size_t DamageTracker::entries() const {
    return entries_.size();
}

std::optional<rendering::rhi::Rect> DamageTracker::previous_bounds(NodeId id) const {
    const auto found = index_.find(id);
    if (found == index_.end()) {
        return std::nullopt;
    }
    return found->second->bounds;
}

} // namespace tessera::ui::layout
