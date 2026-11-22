// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/apply_layout.hpp"
#include "tessera/ui/scene/node.hpp"

#include <algorithm>
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

// The result's boxes as one flat array sorted by id, so a node resolves with a binary search
// instead of a `std::map` build, and the matched ids need no `std::set` (issue #735, slice H).
//
// `LayoutResult::boxes` is in layout order, which a CSS `order` permutation, a grid item or a
// subtree relayout patch can make differ from the scene's tree order, so the sort is what makes the
// lookup independent of that order. Ids are normally assigned in tree order, so the array is
// usually already sorted and the check below skips the sort on a steady frame.
class BoxIndex {
public:
    explicit BoxIndex(const std::vector<LayoutBox>& boxes)
        : entries_(boxes.size()) {
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            entries_[i] = Entry{boxes[i].id, &boxes[i], false};
        }
        if (!std::is_sorted(entries_.begin(), entries_.end(), by_id)) {
            std::sort(entries_.begin(), entries_.end(), by_id);
        }
    }

    // The box for `id`, or nullptr when the result has none. A duplicate id keeps the last box,
    // like the map this replaced.
    [[nodiscard]] const LayoutBox* find(NodeId id) {
        const auto first =
            std::lower_bound(entries_.begin(), entries_.end(), id,
                             [](const Entry& entry, NodeId key) { return entry.id < key; });
        if (first == entries_.end() || first->id != id) {
            return nullptr;
        }
        const LayoutBox* box = nullptr;
        for (auto entry = first; entry != entries_.end() && entry->id == id; ++entry) {
            box = entry->box;
        }
        // One id counts once however many boxes carry it: `extra` is the box count minus the number
        // of distinct ids a node claimed, exactly what the matched-id set reported.
        if (!first->counted) {
            first->counted = true;
            ++matched_;
        }
        return box;
    }

    // Distinct ids a node in the tree claimed.
    [[nodiscard]] std::size_t matched() const {
        return matched_;
    }

private:
    struct Entry {
        NodeId id = 0;
        const LayoutBox* box = nullptr;
        bool counted = false;
    };

    static bool by_id(const Entry& lhs, const Entry& rhs) {
        return lhs.id < rhs.id;
    }

    std::vector<Entry> entries_;
    std::size_t matched_ = 0;
};

void apply_node(Node& node, BoxIndex& boxes, float parent_x, float parent_y,
                LayoutApplyResult& result) {
    const LayoutBox* box = boxes.find(node.id());
    if (box == nullptr) {
        result.missing += clear_text_fit_subtree(node);
        return;
    }

    node.set_bounds(rhi::Rect{box->bounds.x - parent_x, box->bounds.y - parent_y, box->bounds.width,
                              box->bounds.height});
    // The text-fit fact is rewritten on every pass, `Unknown` included, so a node whose box no
    // longer proves a fit stops being cullable in the same frame (issue #490).
    node.set_text_fits_box(box->text_fit == TextFit::Fits);
    ++result.applied;

    for (const auto& child : node.children()) {
        apply_node(*child, boxes, box->bounds.x, box->bounds.y, result);
    }
}

} // namespace

LayoutApplyResult apply_layout(const LayoutResult& result, Node& root) {
    BoxIndex boxes(result.boxes);
    LayoutApplyResult applied;
    apply_node(root, boxes, 0.0f, 0.0f, applied);
    if (result.boxes.size() > boxes.matched()) {
        applied.extra = result.boxes.size() - boxes.matched();
    }
    return applied;
}

} // namespace tessera::ui::layout
