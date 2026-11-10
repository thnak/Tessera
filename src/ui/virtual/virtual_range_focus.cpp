// SPDX-License-Identifier: MIT
//
// Keyed focus/selection and focus-driven realization for `VirtualRange`
// (`docs/specs/virtualization-v1.md` section 7). Split from `virtual_range.cpp` so both translation
// units stay inside the source-file soft limit.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/virtual/virtual_range.hpp"

#include <algorithm>
#include <limits>

namespace tessera::ui::virtualization {

namespace {

using tessera::ui::style::PseudoState;

constexpr std::size_t kUnknownIndex = std::numeric_limits<std::size_t>::max();

} // namespace

bool VirtualRange::set_focus(VirtualItemId key) noexcept {
    if (!keys_.contains(key) || !item_state_.set_focus(key)) {
        return false;
    }
    apply_focus_state();
    return true;
}

bool VirtualRange::clear_focus() noexcept {
    const bool cleared = item_state_.clear_focus();
    apply_focus_state();
    return cleared;
}

ui::Node* VirtualRange::focused_node() const noexcept {
    if (!item_state_.has_focus()) {
        return nullptr;
    }
    const std::optional<std::size_t> index = keys_.find(item_state_.focused_key());
    return index ? realized_node(*index) : nullptr;
}

void VirtualRange::apply_focus_state() noexcept {
    const bool has_focus = item_state_.has_focus();
    const VirtualItemId focused = item_state_.focused_key();
    for (std::size_t position = 0; position < realized_count_; ++position) {
        RealizedItem& item = realized_[position];
        // Node::set_pseudo_state only dirties style when the bit actually changes, so mirroring the
        // whole realized window here is cheap and keeps exactly one realized node focused.
        const bool wants = has_focus && keys_.key_at(item.index) == focused;
        item.lease.node->set_pseudo_state(PseudoState::Focus, wants);
    }
}

VirtualRange::FocusResult VirtualRange::focus_resolved_index(std::size_t index,
                                                             float viewport_extent,
                                                             ScrollAlignment alignment) {
    FocusResult result;
    result.index = index;
    result.key = keys_.key_at(index);
    // Focus before the update so the `apply_focus_state` inside it sets the node bit on realize
    // instead of one update later.
    (void)item_state_.set_focus(result.key);

    const std::expected<ScrollTarget, VirtualRangeError> target =
        scroll_to_index(index, viewport_extent, alignment);
    if (!target) {
        return result;
    }
    const float previous = scroll_offset_;
    const std::expected<UpdateResult, VirtualRangeError> updated =
        update(target->scroll_offset, viewport_extent);
    if (updated) {
        result.scroll_offset = updated->scroll_offset;
        result.realized = realized_node(index) != nullptr;
        result.scrolled = updated->scroll_offset != previous;
        result.clamped = updated->clamped;
        result.truncated = updated->truncated;
        result.built = updated->built;
        result.released = updated->released;
    }
    apply_focus_state();
    return result;
}

std::expected<VirtualRange::FocusResult, VirtualRangeError>
VirtualRange::focus_item(VirtualItemId key, float viewport_extent, ScrollAlignment alignment) {
    const std::optional<std::size_t> index = keys_.find(key);
    if (!index) {
        return std::unexpected(VirtualRangeError::UnknownKey);
    }
    return focus_resolved_index(*index, viewport_extent, alignment);
}

std::size_t VirtualRange::focus_step_origin() const noexcept {
    if (item_state_.has_focus()) {
        if (const std::optional<std::size_t> index = keys_.find(item_state_.focused_key()); index) {
            return *index;
        }
    }
    if (geometry_.item_count() == 0) {
        return 0;
    }
    const ItemGeometry::Range visible = geometry_.visible_range(scroll_offset_, viewport_extent_);
    return visible.empty() ? geometry_.index_at_offset(scroll_offset_) : visible.first;
}

std::expected<VirtualRange::FocusResult, VirtualRangeError>
VirtualRange::step_focus(FocusStep step, float viewport_extent, ScrollAlignment alignment) {
    const std::size_t count = geometry_.item_count();
    if (count == 0) {
        return std::unexpected(VirtualRangeError::IndexOutOfRange);
    }
    const bool had_focus = item_state_.has_focus();
    const std::size_t origin = focus_step_origin();
    std::size_t index = origin;
    switch (step) {
    case FocusStep::Next:
        index = had_focus && origin + 1 < count ? origin + 1 : origin;
        break;
    case FocusStep::Previous:
        index = had_focus && origin > 0 ? origin - 1 : origin;
        break;
    case FocusStep::First:
        index = 0;
        break;
    case FocusStep::Last:
        index = count - 1;
        break;
    }
    return focus_resolved_index(index, viewport_extent, alignment);
}

bool VirtualRange::set_selected(VirtualItemId key, bool selected) noexcept {
    return keys_.contains(key) && item_state_.set_selected(key, selected);
}

bool VirtualRange::toggle_selected(VirtualItemId key) noexcept {
    return keys_.contains(key) && item_state_.toggle_selected(key);
}

bool VirtualRange::is_selected(VirtualItemId key) const noexcept {
    return keys_.contains(key) && item_state_.is_selected(key);
}

bool VirtualRange::clear_selection() noexcept {
    return item_state_.clear_selection();
}

std::size_t VirtualRange::selected_items(std::span<VirtualItemId> out) const noexcept {
    const std::size_t copied = item_state_.copy_selected(out);
    // Selection is keyed, so a data update can leave a key alive in the store but absent from the
    // source; such a key is not in data order and is dropped from the ordered output (the entry
    // itself is removed by the next `sync`).
    std::size_t kept = 0;
    for (std::size_t position = 0; position < copied; ++position) {
        if (keys_.contains(out[position])) {
            out[kept++] = out[position];
        }
    }
    std::sort(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(kept),
              [this](VirtualItemId lhs, VirtualItemId rhs) {
                  return keys_.find(lhs).value_or(kUnknownIndex) <
                         keys_.find(rhs).value_or(kUnknownIndex);
              });
    return kept;
}

} // namespace tessera::ui::virtualization
