// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/apply_layout.hpp"

#include <cstdint>
#include <map>
#include <set>

namespace tessera::ui::layout {
namespace {

// A subtree with no boxes this pass keeps its previous bounds, so it must not keep previous
// text-fit facts either: paint would trust a measurement of a box the node no longer has. Clears
// the hint on `node` and every descendant and returns the size of the subtree, which is the missing
// count the caller reports.
std::size_t clear_text_fit_subtree(Node& node) {
    node.set_text_fits_box(false);
    std::size_t count = 1;
    for (const auto& child : node.children()) {
        count += clear_text_fit_subtree(*child);
    }
    return count;
}

void apply_node(Node& node, const std::map<std::uint64_t, const LayoutBox*>& boxes, float parent_x,
                float parent_y, LayoutApplyResult& result, std::set<std::uint64_t>& matched) {
    const auto found = boxes.find(node.id());
    if (found == boxes.end() || found->second == nullptr) {
        result.missing += clear_text_fit_subtree(node);
        return;
    }

    const LayoutBox& box = *found->second;
    matched.insert(node.id());
    node.set_bounds(rhi::Rect{box.bounds.x - parent_x, box.bounds.y - parent_y, box.bounds.width,
                              box.bounds.height});
    // The text-fit fact is rewritten on every pass, `Unknown` included, so a node whose box no
    // longer proves a fit stops being cullable in the same frame (issue #490).
    node.set_text_fits_box(box.text_fit == TextFit::Fits);
    ++result.applied;

    for (const auto& child : node.children()) {
        apply_node(*child, boxes, box.bounds.x, box.bounds.y, result, matched);
    }
}

} // namespace

LayoutApplyResult apply_layout(const LayoutResult& result, Node& root) {
    std::map<std::uint64_t, const LayoutBox*> boxes;
    for (const LayoutBox& box : result.boxes) {
        boxes[box.id] = &box;
    }

    LayoutApplyResult applied;
    std::set<std::uint64_t> matched;
    apply_node(root, boxes, 0.0f, 0.0f, applied, matched);
    if (result.boxes.size() > matched.size()) {
        applied.extra = result.boxes.size() - matched.size();
    }
    return applied;
}

} // namespace tessera::ui::layout
