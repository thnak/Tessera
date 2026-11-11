// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/layout_scheduler.hpp"
#include "tessera/ui/layout/layout_signature.hpp"

#include <unordered_set>

namespace tessera::ui::layout {

LayoutScheduler::LayoutScheduler(LayoutEngine& engine)
    : engine_(&engine) {}

const LayoutResult& LayoutScheduler::update(const LayoutInput& root, const LayoutViewport& viewport,
                                            [[maybe_unused]] bool needs_layout) {
    // The signature is authoritative; `needs_layout` only documents caller intent. A paint-only
    // style change can raise layout dirt while the inputs that decide boxes are unchanged, and
    // that report is safe to drop. The hash walk is allocation-free (layout_signature.hpp).
    const std::uint64_t signature = layout_signature(root, viewport);
    if (valid_ && signature_valid_ && signature == signature_) {
        ++cache_hits_;
        return last_;
    }

    last_ = engine_->layout(root, viewport);
    signature_ = signature;
    signature_valid_ = true;
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
    // The patch is not a full reflow: it resolved the subtree in isolation and kept every sibling
    // box as-is. Dropping the signature forces the next update through a real reflow so a later
    // hit can never stand for an approximation.
    signature_valid_ = false;
    return true;
}

void LayoutScheduler::invalidate() {
    valid_ = false;
    last_ = LayoutResult{};
    signature_ = 0;
    signature_valid_ = false;
}

} // namespace tessera::ui::layout
