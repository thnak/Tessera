// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/layout_scheduler.hpp"

#include <unordered_set>

namespace tessera::ui::layout {

LayoutScheduler::LayoutScheduler(LayoutEngine& engine)
    : engine_(&engine) {}

const LayoutResult& LayoutScheduler::update(const LayoutInput& root, const LayoutViewport& viewport,
                                            bool needs_layout) {
    const bool viewport_changed = viewport.width != viewport_.width ||
                                  viewport.height != viewport_.height ||
                                  viewport.dpi_scale != viewport_.dpi_scale;
    if (valid_ && !needs_layout && !viewport_changed) {
        ++cache_hits_;
        return last_;
    }

    last_ = engine_->layout(root, viewport);
    viewport_ = viewport;
    valid_ = true;
    ++layout_runs_;
    return last_;
}

bool LayoutScheduler::relayout_subtree(const LayoutInput& root, std::uint64_t id,
                                       const LayoutViewport& viewport) {
    if (!valid_) {
        return false;
    }
    const LayoutBox* previous = nullptr;
    for (const LayoutBox& box : last_.boxes) {
        if (box.id == id) {
            previous = &box;
            break;
        }
    }
    if (previous == nullptr) {
        return false;
    }
    const float offset_x = previous->bounds.x;
    const float offset_y = previous->bounds.y;

    LayoutResult subtree = engine_->layout_subtree(root, id, viewport);
    if (subtree.boxes.empty()) {
        return false;
    }

    // The subtree layout starts its own root at (0,0), but margins may still apply to it. Map
    // the subtree root back onto the previous root box instead of assuming a zero origin.
    const LayoutBox* subtree_root = nullptr;
    for (const LayoutBox& box : subtree.boxes) {
        if (box.id == id) {
            subtree_root = &box;
            break;
        }
    }
    if (subtree_root == nullptr) {
        return false;
    }
    const float delta_x = offset_x - subtree_root->bounds.x;
    const float delta_y = offset_y - subtree_root->bounds.y;

    std::unordered_set<std::uint64_t> ids;
    ids.reserve(subtree.boxes.size());
    for (const LayoutBox& box : subtree.boxes) {
        ids.insert(box.id);
    }
    std::erase_if(last_.boxes, [&ids](const LayoutBox& box) { return ids.contains(box.id); });
    for (LayoutBox& box : subtree.boxes) {
        box.bounds.x += delta_x;
        box.bounds.y += delta_y;
        last_.boxes.push_back(box);
    }
    ++partial_runs_;
    return true;
}

void LayoutScheduler::invalidate() {
    valid_ = false;
    last_ = LayoutResult{};
}

} // namespace tessera::ui::layout
