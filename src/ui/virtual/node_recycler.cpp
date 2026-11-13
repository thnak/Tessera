// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/virtual/node_recycler.hpp"

#include <algorithm>
#include <cassert>
#include <string_view>

namespace tessera::ui::virtualization {

namespace {

using tessera::ui::Dirty;
using tessera::ui::Node;
using tessera::ui::style::PseudoState;

[[nodiscard]] bool is_identity(const tessera::ui::rhi::Mat3& transform) noexcept {
    const tessera::ui::rhi::Mat3 identity{};
    for (std::size_t index = 0; index < 9; ++index) {
        if (transform.m[index] != identity.m[index]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_zero_rect(const tessera::ui::rhi::Rect& rect) noexcept {
    return rect.x == 0.0f && rect.y == 0.0f && rect.width == 0.0f && rect.height == 0.0f;
}

[[nodiscard]] bool has_paint_outsets(const Node& node) noexcept {
    const Node::PaintOutsets& outsets = node.paint_outsets();
    return outsets.left != 0.0f || outsets.top != 0.0f || outsets.right != 0.0f ||
           outsets.bottom != 0.0f;
}

} // namespace

bool node_carries_item_state(const Node& node) noexcept {
    if (node.parent() != nullptr || !node.children().empty()) {
        return true;
    }
    if (node.paint_data().has_value()) {
        return true;
    }
    if (!node.element_kind().empty() || !node.element_id().empty() || !node.class_name().empty() ||
        !node.part().empty() || !node.component_name().empty() || !node.role().empty() ||
        node.automation_id() != 0) {
        return true;
    }
    if (!node.aria_attributes().empty() || !node.inline_style().empty() ||
        !node.event_bindings().empty()) {
        return true;
    }
    if (!node.visible() || node.focusable() || node.tab_index() != 0 || node.z_index() != 0 ||
        node.pseudo_state() != PseudoState::None) {
        return true;
    }
    if (node.scrollable_x() || node.scrollable_y() || node.scroll_x() != 0.0f ||
        node.scroll_y() != 0.0f || node.content_width() != 0.0f || node.content_height() != 0.0f ||
        node.has_scrollport_insets()) {
        return true;
    }
    if (has_paint_outsets(node) || !is_identity(node.transform()) || !is_zero_rect(node.bounds())) {
        return true;
    }
    // A pending dirty domain is scene work, not item identity, but a pooled node must be idle so
    // the next build starts from a clean slate.
    return node.dirty() != Dirty::Clean;
}

void clear_node_item_state(Node& node) noexcept {
    // Built children belong to the previous item; the next build creates its own subtree.
    while (!node.children().empty()) {
        (void)node.remove_child(*node.children().back());
    }
    node.clear_paint_data();
    node.set_element_kind(std::string_view{});
    node.clear_element_id();
    node.clear_class_name();
    node.clear_part();
    node.clear_component_name();
    node.clear_role();
    node.set_automation_id(0);
    node.clear_aria();
    node.clear_inline_style();
    node.clear_event_bindings();
    node.set_pseudo_state(PseudoState::None);
    node.set_visible(true);
    node.set_focusable(false);
    node.set_tab_index(0);
    node.set_z_index(0);
    node.set_paint_outsets(Node::PaintOutsets{});
    node.set_scrollable_x(false);
    node.set_scrollable_y(false);
    node.set_content_size(0.0f, 0.0f);
    node.set_scroll_offset(0.0f, 0.0f);
    node.set_scrollport_insets(0.0f, 0.0f, 0.0f, 0.0f);
    node.set_transform(tessera::ui::rhi::Mat3::identity());
    node.set_bounds(tessera::ui::rhi::Rect{});
    node.clear_dirty(Dirty::All);
}

NodeRecycler::NodeRecycler(std::size_t pool_cap)
    : capacity_(std::max(pool_cap, kMinVirtualListPoolCap)),
      slots_(capacity_),
      free_ring_(capacity_, 0) {}

NodeRecycler::~NodeRecycler() = default;

std::optional<NodeRecycler::Lease> NodeRecycler::acquire() {
    std::uint32_t slot = kInvalidSlot;
    if (free_count_ > 0) {
        slot = free_ring_[free_head_];
        free_head_ = (free_head_ + 1) % capacity_;
        --free_count_;
        ++stats_.pooled_acquires;
    } else if (created_ < capacity_) {
        slot = static_cast<std::uint32_t>(created_);
        slots_[slot].node = std::make_unique<Node>();
        ++created_;
        ++stats_.allocations;
    } else {
        // Cap enforcement is a refusal, never a growth or an eviction.
        ++stats_.denied;
        return std::nullopt;
    }

    Slot& entry = slots_[slot];
    ++entry.generation;
    entry.state = SlotState::Owned;
    entry.leased = entry.node.get();
    ++in_use_;
    ++stats_.acquires;

    clear_node_item_state(*entry.node);
#ifndef NDEBUG
    assert(!node_carries_item_state(*entry.node) &&
           "virtualization: acquire returned a node that still carries item state");
#endif
    return Lease{entry.node.get(), slot, entry.generation};
}

std::unique_ptr<Node> NodeRecycler::detach(Lease lease) noexcept {
    if (!matches(lease) || slots_[lease.slot].state != SlotState::Owned) {
        ++stats_.stale_leases;
        return nullptr;
    }
    Slot& entry = slots_[lease.slot];
    entry.state = SlotState::Detached;
    ++stats_.detaches;
    return std::move(entry.node);
}

bool NodeRecycler::release(Lease lease) noexcept {
    if (!matches(lease) || slots_[lease.slot].state != SlotState::Owned) {
        ++stats_.stale_leases;
        return false;
    }
    Slot& entry = slots_[lease.slot];
    // The contract is that the component/data-source release path cleared item state first. A
    // violation is counted and asserted in debug; release builds still sanitize below so the
    // "clean on acquire" guarantee never depends on NDEBUG.
    if (node_carries_item_state(*entry.node)) {
        ++stats_.clean_violations;
#ifndef NDEBUG
        assert(false && "virtualization: released item node still carries item state");
#endif
    }
    clear_node_item_state(*entry.node);
    ++stats_.releases;
    push_free(lease.slot);
    return true;
}

bool NodeRecycler::reclaim(Lease lease, std::unique_ptr<Node> owned) noexcept {
    if (!owned || !matches(lease) || slots_[lease.slot].state != SlotState::Detached ||
        owned.get() != lease.node) {
        ++stats_.stale_leases;
        return false;
    }
    Slot& entry = slots_[lease.slot];
    if (node_carries_item_state(*owned)) {
        ++stats_.clean_violations;
#ifndef NDEBUG
        assert(false && "virtualization: reclaimed item node still carries item state");
#endif
    }
    clear_node_item_state(*owned);
    entry.node = std::move(owned);
    ++stats_.reclaims;
    push_free(lease.slot);
    return true;
}

bool NodeRecycler::matches(Lease lease) const noexcept {
    if (lease.slot == kInvalidSlot || lease.slot >= created_ || lease.node == nullptr) {
        return false;
    }
    const Slot& entry = slots_[lease.slot];
    return entry.leased == lease.node && entry.generation == lease.generation;
}

void NodeRecycler::push_free(std::uint32_t slot) noexcept {
    Slot& entry = slots_[slot];
    entry.state = SlotState::Free;
    entry.leased = nullptr;
    // FIFO tail: the oldest freed slot is at free_head_, matching the deterministic release order.
    free_ring_[(free_head_ + free_count_) % capacity_] = slot;
    ++free_count_;
    --in_use_;
}

} // namespace tessera::ui::virtualization
