// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/damage.hpp"
#include "tessera/ui/scene/dirty_pass.hpp"

#include <cstddef>
#include <list>
#include <optional>
#include <unordered_map>
#include <vector>

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

// The paint dependency region of a glass node in device pixels: its box grown by 3 * sigma
// (docs/specs/glass-v1.md section 7). Damage that intersects this region must repaint the glass
// node; damage outside it cannot change a blurred pixel. Empty for a node with no backdrop-filter.
[[nodiscard]] rendering::rhi::Rect node_glass_dependency_rect(const Node& node,
                                                              float dpi_scale = 1.0f);

// The region a filtered node owns, in device pixels: its subtree paint box (the node, its children
// and the filter's halo around both) mapped to the world. It is both the node's dependency region
// and the box that must be repainted whole when damage touches it, because a filtered pixel anywhere
// in it may depend on the changed one. Empty for a node with no layer `filter`.
[[nodiscard]] rendering::rhi::Rect node_filter_region(const Node& node, float dpi_scale = 1.0f);

class DamageTracker {
public:
    explicit DamageTracker(std::size_t max_entries = 4096);

    DamagePassResult add_scene_damage(const DirtyPass& dirty, rendering::DamageAccumulator& damage,
                                      float dpi_scale = 1.0f, float margin = 0.0f);

    // Grows accumulated damage so glass nodes stay correct (docs/specs/glass-v1.md section 7):
    // every glass node whose 3 sigma dependency region intersects the damage already accumulated
    // gets its full box added, because a blurred pixel anywhere in the box may have changed. A tree
    // with no glass node does nothing (the subtree flag prunes the walk), so non-glass scenes are
    // unchanged. Returns how many glass nodes were added. Call it after every damage source of the
    // frame has been accumulated and before the redraw plan is built.
    std::size_t add_glass_dependencies(const Node& root, rendering::DamageAccumulator& damage,
                                       float dpi_scale = 1.0f, float margin = 0.0f);

    // The same rule for layer filters (docs/design/gpu/offscreen-layers.md section 5d): a filtered
    // node whose region (`node_filter_region`) intersects the accumulated damage gets its whole
    // region added, because the filter reads pixels up to its reach away. Pruned by the node's
    // `filter_subtree` flag, independent of glass, and run before the glass walk so a backdrop
    // blur sees the filter's repaint. Returns how many filtered nodes were added.
    std::size_t add_filter_dependencies(const Node& root, rendering::DamageAccumulator& damage,
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
    // Reused scratch for add_glass_dependencies(), so a steady glass frame does not allocate.
    std::vector<rendering::rhi::Rect> damage_scratch_;
    std::vector<rendering::rhi::Rect> glass_scratch_;
};

} // namespace tessera::ui::layout
