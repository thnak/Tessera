// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/apply_layout.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstddef>
#include <vector>

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

// The result's boxes in one flat open-addressing table keyed by id, so a node resolves with one
// probe instead of a binary search over a sorted copy, and the matched ids need no `std::set`
// (issue #735, slice H).
//
// `LayoutResult::boxes` is in layout order, which a CSS `order` permutation, a grid item or a
// subtree relayout patch can make differ from the scene's tree order, and ids are only normally
// assigned in tree order (a list that grows later hands out larger ids to earlier positions). A
// sorted array therefore had to be sorted again every frame on such a scene - about 10 percent of
// the incremental frame at 12k nodes - while a table is built in one pass whatever the order.
class BoxIndex {
public:
    explicit BoxIndex(const std::vector<LayoutBox>& boxes) {
        std::size_t capacity = 16;
        while (capacity < boxes.size() * 2) {
            capacity <<= 1;
        }
        slots_.resize(capacity);
        mask_ = capacity - 1;
        for (const LayoutBox& box : boxes) {
            Slot& slot = slot_for(box.id);
            slot.id = box.id;
            slot.box = &box; // a duplicate id keeps the last box, like the map this replaced
        }
    }

    // The box for `id`, or nullptr when the result has none. One id counts once however many boxes
    // carry it: `extra` is the box count minus the number of distinct ids a node claimed, exactly
    // what the matched-id set reported.
    [[nodiscard]] const LayoutBox* find(NodeId id) {
        Slot& slot = slot_for(id);
        if (slot.box == nullptr) {
            return nullptr;
        }
        if (!slot.counted) {
            slot.counted = true;
            ++matched_;
        }
        return slot.box;
    }

    // Distinct ids a node in the tree claimed.
    [[nodiscard]] std::size_t matched() const {
        return matched_;
    }

private:
    struct Slot {
        NodeId id = 0;
        const LayoutBox* box = nullptr; // null marks an empty slot
        bool counted = false;
    };

    // The slot holding `id`, or the empty slot it would be inserted into. The table is at most half
    // full, so a probe sequence always ends.
    Slot& slot_for(NodeId id) {
        std::size_t index = static_cast<std::size_t>(id * 0x9E3779B97F4A7C15ULL >> 32) & mask_;
        while (slots_[index].box != nullptr && slots_[index].id != id) {
            index = (index + 1) & mask_;
        }
        return slots_[index];
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    std::size_t matched_ = 0;
};

void apply_node(Node& node, BoxIndex& boxes, float parent_x, float parent_y,
                LayoutApplyResult& result, std::vector<const Node*>* resized,
                bool ancestor_resized) {
    const LayoutBox* box = boxes.find(node.id());
    if (box == nullptr) {
        result.missing += clear_text_fit_subtree(node);
        return;
    }

    bool size_changed = false;
    if (resized != nullptr) {
        const rhi::Rect& old_bounds = node.bounds();
        size_changed =
            old_bounds.width != box->bounds.width || old_bounds.height != box->bounds.height;
        if (size_changed && !ancestor_resized) {
            resized->push_back(&node);
        }
    }
    node.set_bounds(rhi::Rect{box->bounds.x - parent_x, box->bounds.y - parent_y, box->bounds.width,
                              box->bounds.height});
    // The text-fit fact is rewritten on every pass, `Unknown` included, so a node whose box no
    // longer proves a fit stops being cullable in the same frame (issue #490).
    node.set_text_fits_box(box->text_fit == TextFit::Fits);
    ++result.applied;

    for (const auto& child : node.children()) {
        apply_node(*child, boxes, box->bounds.x, box->bounds.y, result, resized,
                   ancestor_resized || size_changed);
    }
}

} // namespace

namespace {

LayoutApplyResult apply_impl(const LayoutResult& result, Node& root,
                             std::vector<const Node*>* resized) {
    BoxIndex boxes(result.boxes);
    LayoutApplyResult applied;
    apply_node(root, boxes, 0.0f, 0.0f, applied, resized, false);
    if (result.boxes.size() > boxes.matched()) {
        applied.extra = result.boxes.size() - boxes.matched();
    }
    return applied;
}

} // namespace

LayoutApplyResult apply_layout(const LayoutResult& result, Node& root) {
    return apply_impl(result, root, nullptr);
}

LayoutApplyResult apply_layout(const LayoutResult& result, Node& root,
                               std::vector<const Node*>& resized) {
    resized.clear();
    return apply_impl(result, root, &resized);
}

} // namespace tessera::ui::layout
