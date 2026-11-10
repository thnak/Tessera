// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/virtual/virtual_range.hpp"

#include <algorithm>
#include <cassert>
#include <limits>

namespace tessera::ui::virtualization {

namespace {

[[nodiscard]] VirtualRangeError map_key_error(KeyIndexError::Kind kind) noexcept {
    return kind == KeyIndexError::Kind::DuplicateKey ? VirtualRangeError::DuplicateKey
                                                     : VirtualRangeError::CapacityExceeded;
}

} // namespace

VirtualRange::VirtualRange(Config config, VirtualDataSource& source, ui::Node& container)
    : config_(config),
      source_(&source),
      container_(&container),
      geometry_(ItemGeometry::Config{
          .capacity = config.capacity, .estimate_main = config.estimate_main, .gap = config.gap}),
      keys_(config.capacity),
      recycler_(config.pool_cap),
      item_state_(config.state_capacity),
      realized_(recycler_.capacity()) {
    config_.overscan_main = std::max(config_.overscan_main, 0.0f);
    config_.cross_extent = std::max(config_.cross_extent, 0.0f);
}

VirtualRange::~VirtualRange() {
    // The container may outlive the range; return its item children to the pool. The source is not
    // notified because the range no longer knows whether the data behind those indices exists.
    release_all(false);
}

std::expected<void, VirtualRangeError> VirtualRange::sync() {
    release_all(false);
    has_anchor_ = false;
    scroll_offset_ = 0.0f;

    const auto rebuilt = keys_.rebuild(*source_);
    const std::size_t count = source_->item_count();
    if (!rebuilt) {
        (void)geometry_.set_count(0);
        // The key mapping is invalid, so every keyed state entry is stale; dropping it also clears
        // focus/selection instead of leaving them pointing at keys the collection cannot resolve.
        item_state_.retain_if([this](VirtualItemId key) { return keys_.contains(key); });
        update_container_content_size();
        return std::unexpected(map_key_error(rebuilt.error().kind));
    }
    if (!geometry_.set_count(count)) {
        (void)geometry_.set_count(0);
        item_state_.retain_if([this](VirtualItemId key) { return keys_.contains(key); });
        update_container_content_size();
        return std::unexpected(VirtualRangeError::CapacityExceeded);
    }
    // Measurements are index-addressed, so any data change invalidates them; keeping them across
    // an insert would attach one item's height to its neighbour. The next update measures the
    // window afresh (spec section 4: inserts/removes update the prefix index, no full relayout).
    geometry_.clear_measurements();
    // Keys, not indices, carry identity: focus/selection for keys the source still has survive the
    // data change (spec section 11), and entries for vanished keys are dropped.
    item_state_.retain_if([this](VirtualItemId key) { return keys_.contains(key); });
    update_container_content_size();
    return {};
}

std::expected<VirtualRange::UpdateResult, VirtualRangeError>
VirtualRange::update(float scroll_offset, float viewport_extent) {
    UpdateResult result;
    viewport_extent_ = std::max(viewport_extent, 0.0f);

    const float maximum = std::max(geometry_.total_extent() - viewport_extent_, 0.0f);
    scroll_offset_ = std::clamp(scroll_offset, 0.0f, maximum);
    result.clamped = scroll_offset_ != scroll_offset;

    const ItemGeometry::Range window = expanded_window(scroll_offset_, viewport_extent_);
    result.window = window;
    if (window.empty()) {
        result.released = 0;
        while (realized_count_ > 0) {
            release_at(0, true);
            ++result.released;
        }
        has_anchor_ = false;
        update_container_content_size();
        result.scroll_offset = scroll_offset_;
        return result;
    }

    // The anchor is the first *visible* item, not the first overscanned one: measuring an item
    // between the overscan edge and the viewport start must still scroll the anchor back into
    // place.
    const ItemGeometry::Range visible = geometry_.visible_range(scroll_offset_, viewport_extent_);
    set_anchor_from(visible.empty() ? window.first : visible.first);

    // Release first so a forward scroll can immediately reuse the nodes it just freed.
    for (std::size_t position = 0; position < realized_count_;) {
        const std::size_t index = realized_[position].index;
        if (index < window.first || index >= window.last) {
            release_at(position, true);
            ++result.released;
        } else {
            ++position;
        }
    }

    const std::size_t allocations_before = recycler_.stats().allocations;
    for (std::size_t index = window.first; index < window.last; ++index) {
        if (realized_node(index) != nullptr) {
            continue;
        }
        const std::optional<NodeRecycler::Lease> lease = recycler_.acquire();
        if (!lease) {
            // The window is wider than the node pool: realize the prefix that fits and report it
            // instead of growing the pool or evicting a visible node.
            result.truncated = true;
            break;
        }
        const VirtualItemMeasure measure =
            source_->measure(index, MeasureConstraints{config_.cross_extent});
        const Refinement refinement = refine_and_reanchor(index, measure.main_size);
        if (refinement.change.changed) {
            ++result.measured;
            result.shifted = result.shifted || refinement.adjustment.shifted;
            result.clamped = result.clamped || refinement.adjustment.clamped;
        }

        source_->build(index, *lease->node);
        std::unique_ptr<ui::Node> owned = recycler_.detach(*lease);
        if (!owned) {
            break; // unreachable with a fresh lease; never leave the container half-attached
        }
        container_->add_child(std::move(owned));
        realized_[realized_count_] = RealizedItem{index, *lease, measure.cross_size};
        ++realized_count_;
        ++result.built;
    }
    result.reused = result.built - (recycler_.stats().allocations - allocations_before);

    // Refinements clamp the scroll, but an unchanged last refinement does not; clamp once more so
    // the container never reports a scroll past the freshly measured end.
    const float measured_maximum = std::max(geometry_.total_extent() - viewport_extent_, 0.0f);
    if (scroll_offset_ > measured_maximum) {
        scroll_offset_ = measured_maximum;
        result.clamped = true;
    }

    place_realized_nodes();
    update_container_content_size();
    apply_focus_state();

    if (realized_count_ == 0) {
        result.realized = ItemGeometry::Range{0, 0};
    } else {
        std::size_t first = std::numeric_limits<std::size_t>::max();
        std::size_t last = 0;
        for (std::size_t position = 0; position < realized_count_; ++position) {
            const std::size_t index = realized_[position].index;
            first = std::min(first, index);
            last = std::max(last, index);
        }
        result.realized = ItemGeometry::Range{first, last + 1};
    }
    result.scroll_offset = scroll_offset_;
    return result;
}

ItemGeometry::Range VirtualRange::expanded_window(float scroll_offset,
                                                  float viewport_extent) const noexcept {
    const std::size_t count = geometry_.item_count();
    const float total = geometry_.total_extent();
    if (count == 0 || viewport_extent <= 0.0f || total <= 0.0f) {
        return ItemGeometry::Range{0, 0};
    }
    const float start = std::max(scroll_offset - config_.overscan_main, 0.0f);
    if (start >= total) {
        return ItemGeometry::Range{count, count};
    }
    const float end = scroll_offset + viewport_extent + config_.overscan_main;
    const std::size_t first = geometry_.index_at_offset(start);
    const std::size_t last =
        end >= total ? count : std::min(geometry_.index_at_offset(end) + 1, count);
    return ItemGeometry::Range{first, std::max(first, last)};
}

void VirtualRange::set_anchor_from(std::size_t index) noexcept {
    anchor_.key = keys_.key_at(index);
    anchor_.offset_in_item = std::max(scroll_offset_ - geometry_.item_offset(index), 0.0f);
    has_anchor_ = true;
}

std::size_t VirtualRange::resolve_anchor_index() const noexcept {
    if (has_anchor_) {
        if (const std::optional<std::size_t> index = keys_.find(anchor_.key); index) {
            return *index;
        }
    }
    if (geometry_.item_count() == 0) {
        return 0;
    }
    return geometry_.index_at_offset(scroll_offset_);
}

VirtualRange::Refinement VirtualRange::refine_and_reanchor(std::size_t index,
                                                           float main_size) noexcept {
    Refinement refinement;
    refinement.change = geometry_.refine_measure(index, main_size);
    refinement.adjustment.scroll_offset = scroll_offset_;
    if (!refinement.change.changed) {
        return refinement;
    }
    refinement.adjustment =
        reanchor_scroll(scroll_offset_, viewport_extent_, geometry_.total_extent(),
                        resolve_anchor_index(), index, refinement.change.delta);
    scroll_offset_ = refinement.adjustment.scroll_offset;
    return refinement;
}

void VirtualRange::release_at(std::size_t position, bool notify_source) noexcept {
    RealizedItem item = realized_[position];
    std::unique_ptr<ui::Node> owned = container_->remove_child(*item.lease.node);
#ifndef NDEBUG
    assert(owned != nullptr && "virtualization: realized item is not a child of the container");
#endif
    if (owned) {
        if (notify_source) {
            source_->release(item.index, *owned);
        }
        clear_node_item_state(*owned);
        (void)recycler_.reclaim(item.lease, std::move(owned));
    }
    realized_[position] = realized_[realized_count_ - 1];
    realized_[realized_count_ - 1] = RealizedItem{};
    --realized_count_;
}

void VirtualRange::release_all(bool notify_source) noexcept {
    while (realized_count_ > 0) {
        release_at(realized_count_ - 1, notify_source);
    }
}

void VirtualRange::place_realized_nodes() noexcept {
    for (std::size_t position = 0; position < realized_count_; ++position) {
        RealizedItem& item = realized_[position];
        const float offset = geometry_.item_offset(item.index) - scroll_offset_;
        const float main_size = geometry_.item_main_size(item.index);
        if (config_.horizontal) {
            item.lease.node->set_bounds(
                tessera::ui::rhi::Rect{offset, 0.0f, main_size, item.cross_size});
        } else {
            item.lease.node->set_bounds(
                tessera::ui::rhi::Rect{0.0f, offset, item.cross_size, main_size});
        }
    }
}

void VirtualRange::update_container_content_size() noexcept {
    const float total = geometry_.total_extent();
    if (config_.horizontal) {
        container_->set_content_size(total, config_.cross_extent);
    } else {
        container_->set_content_size(config_.cross_extent, total);
    }
}

} // namespace tessera::ui::virtualization
