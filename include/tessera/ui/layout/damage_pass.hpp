// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/damage.hpp"
#include "tessera/ui/scene/dirty_pass.hpp"

#include <cstddef>
#include <list>
#include <optional>
#include <unordered_map>

namespace tessera::ui::layout {

struct DamagePassResult {
    std::size_t nodes = 0;
    std::size_t rects = 0;
    bool full_redraw = false;
    bool overflowed = false;
};

DamagePassResult add_scene_damage(const DirtyPass& dirty, rendering::DamageAccumulator& damage,
                                  float dpi_scale = 1.0f, float margin = 0.0f);

// Paint-side damage rect for a node: its world bounds (grown by Node::paint_outsets for shadows)
// for every kind. Text is deliberately not
// tightened (docs/design/dirty-rendering.md 8.2). Callers snap the rect themselves.
[[nodiscard]] rendering::rhi::Rect node_damage_rect(const Node& node);

class DamageTracker {
public:
    explicit DamageTracker(std::size_t max_entries = 4096);

    DamagePassResult add_scene_damage(const DirtyPass& dirty, rendering::DamageAccumulator& damage,
                                      float dpi_scale = 1.0f, float margin = 0.0f);

    std::size_t prune_removed(const Node& root);
    void clear();

    [[nodiscard]] std::size_t entries() const;
    [[nodiscard]] std::size_t capacity() const {
        return capacity_;
    }
    [[nodiscard]] std::optional<rendering::rhi::Rect> previous_bounds(NodeId id) const;

private:
    struct Entry {
        NodeId id = 0;
        rendering::rhi::Rect bounds;
    };

    std::list<Entry> entries_;
    std::unordered_map<NodeId, std::list<Entry>::iterator> index_;
    std::size_t capacity_ = 0;
};

} // namespace tessera::ui::layout
