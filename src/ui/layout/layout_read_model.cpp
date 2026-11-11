// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/layout_read_model.hpp"

#include <algorithm>
#include <cassert>

namespace tessera::ui::layout {
namespace {

rhi::Rect content_box_of(const LayoutBox& box) {
    const LayoutInsets& inset = box.content_insets;
    const float width = std::max(0.0f, box.bounds.width - inset.left - inset.right);
    const float height = std::max(0.0f, box.bounds.height - inset.top - inset.bottom);
    return rhi::Rect{box.bounds.x + inset.left, box.bounds.y + inset.top, width, height};
}

} // namespace

void LayoutReadModel::publish(const LayoutResult& result, float dpi_scale) {
    assert(std::this_thread::get_id() == owner_ &&
           "LayoutReadModel::publish must run on the UI thread");
    entries_.clear(); // keeps capacity: a steady tree publishes without allocating
    entries_.reserve(result.boxes.size());
    for (const LayoutBox& box : result.boxes) {
        entries_.push_back(Entry{box.id, box.bounds, content_box_of(box)});
    }
    std::sort(entries_.begin(), entries_.end(),
              [](const Entry& lhs, const Entry& rhs) { return lhs.id < rhs.id; });
    dpi_scale_ = dpi_scale;
    ++generation_;
}

std::optional<LayoutInfo> LayoutReadModel::find(const Node& node) const {
    assert(std::this_thread::get_id() == owner_ &&
           "LayoutReadModel::find must run on the UI thread");
    const std::uint64_t id = node.id();
    const auto found =
        std::lower_bound(entries_.begin(), entries_.end(), id,
                         [](const Entry& entry, std::uint64_t key) { return entry.id < key; });
    if (found == entries_.end() || found->id != id) {
        return std::nullopt;
    }
    return LayoutInfo{found->border_box, found->content_box, node.world_bounds(), dpi_scale_,
                      generation_};
}

} // namespace tessera::ui::layout
