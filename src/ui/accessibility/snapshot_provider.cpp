// SPDX-License-Identifier: MIT
//
// The pull-side accessibility provider Android needs (issue #189). See snapshot_provider.hpp for
// why it exists and how the threading is bounded.

#include "tessera/ui/accessibility/snapshot_provider.hpp"

#include <algorithm>
#include <cstring>

namespace tessera::ui::a11y {
namespace {

constexpr std::uint16_t kFreeSlot = static_cast<std::uint16_t>(kMaxNodes);

} // namespace

SnapshotProvider::SnapshotProvider() {
    nodes_.nodes.reserve(kMaxNodes);
    clear();
}

void SnapshotProvider::clear() {
    std::lock_guard lock(mutex_);
    nodes_.nodes.clear();
    slot_of_node_.fill(0);
    index_of_slot_.fill(kFreeSlot);
    virtual_id_of_index_.fill(kHostVirtualId);
    node_count_ = 0;
    action_count_ = 0;
    has_snapshot_ = false;
}

void SnapshotProvider::publish(const A11ySnapshot& snapshot) {
    std::lock_guard lock(mutex_);
    // `assign` reuses the retained vector's capacity, so a steady-state publish does not allocate.
    nodes_.nodes.assign(snapshot.nodes.begin(), snapshot.nodes.end());
    // A snapshot can carry more than kMaxNodes only if a caller ignored the builder's cap; the
    // arrays below are sized for the cap, so the extra entries are dropped rather than overrunning.
    if (nodes_.nodes.size() > kMaxNodes) {
        nodes_.nodes.resize(kMaxNodes);
    }
    node_count_ = nodes_.nodes.size();
    assign_slots();
    has_snapshot_ = true;
    ++publishes_;
}

void SnapshotProvider::assign_slots() {
    // Keep the virtual id a scene node already had: a screen reader that focused virtual view 3
    // must still find the same control after a rebuild that reordered the pre-order walk. The old
    // mapping is read before it is cleared, and a node that left the tree frees its slot for a new
    // node in the same pass.
    std::array<ui::NodeId, kMaxNodes + 1> previous = slot_of_node_;
    slot_of_node_.fill(0);
    index_of_slot_.fill(kFreeSlot);
    virtual_id_of_index_.fill(kHostVirtualId);

    // Pass one: nodes that already had a slot keep it.
    for (std::size_t index = 0; index < node_count_; ++index) {
        const ui::NodeId id = nodes_.nodes[index].id;
        for (std::size_t slot = 1; slot <= kMaxNodes; ++slot) {
            if (previous[slot] == id) {
                slot_of_node_[slot] = id;
                index_of_slot_[slot] = static_cast<std::uint16_t>(index);
                virtual_id_of_index_[index] = static_cast<int>(slot);
                break;
            }
        }
    }
    // Pass two: every node still without a slot takes the lowest free one, in pre-order, so a
    // fresh tree numbers top-to-bottom the way a screen reader reads it.
    std::size_t next_free = 1;
    for (std::size_t index = 0; index < node_count_; ++index) {
        if (virtual_id_of_index_[index] != kHostVirtualId) {
            continue;
        }
        while (next_free <= kMaxNodes && index_of_slot_[next_free] != kFreeSlot) {
            ++next_free;
        }
        if (next_free > kMaxNodes) {
            // More nodes than slots. kMaxNodes nodes and kMaxNodes slots means this is unreachable
            // unless a snapshot repeated a node id, which the builder does not do; the node is left
            // unaddressed rather than aliasing another.
            break;
        }
        const std::size_t slot = next_free;
        slot_of_node_[slot] = nodes_.nodes[index].id;
        index_of_slot_[slot] = static_cast<std::uint16_t>(index);
        virtual_id_of_index_[index] = static_cast<int>(slot);
        ++next_free;
    }
}

std::size_t SnapshotProvider::index_of(int virtual_id) const {
    if (virtual_id <= kHostVirtualId || static_cast<std::size_t>(virtual_id) > kMaxNodes) {
        return kMaxNodes;
    }
    const std::uint16_t index = index_of_slot_[static_cast<std::size_t>(virtual_id)];
    if (index == kFreeSlot || index >= node_count_) {
        return kMaxNodes;
    }
    return index;
}

std::size_t SnapshotProvider::count() const {
    std::lock_guard lock(mutex_);
    return node_count_;
}

bool SnapshotProvider::has_snapshot() const {
    std::lock_guard lock(mutex_);
    return has_snapshot_;
}

std::uint64_t SnapshotProvider::publishes() const {
    std::lock_guard lock(mutex_);
    return publishes_;
}

std::uint64_t SnapshotProvider::dropped_actions() const {
    std::lock_guard lock(mutex_);
    return dropped_actions_;
}

int SnapshotProvider::focused_virtual_id() const {
    std::lock_guard lock(mutex_);
    for (std::size_t index = 0; index < node_count_; ++index) {
        if (has_state(nodes_.nodes[index].states, State::Focused)) {
            return virtual_id_of_index_[index];
        }
    }
    return kHostVirtualId;
}

ui::NodeId SnapshotProvider::node_of(int virtual_id) const {
    std::lock_guard lock(mutex_);
    const std::size_t index = index_of(virtual_id);
    if (index == kMaxNodes) {
        return 0;
    }
    return nodes_.nodes[index].id;
}

bool SnapshotProvider::describe(int virtual_id, ProviderAnswer& out) const {
    out = ProviderAnswer{};
    std::lock_guard lock(mutex_);
    // The host view is the window, not a snapshot node: it has no class name of its own. A false
    // answer here is what makes the platform fall back to the host's own description.
    const std::size_t index = index_of(virtual_id);
    if (index == kMaxNodes) {
        return false;
    }
    const A11yNode& node = nodes_.nodes[index];
    out.role = node.role;
    out.node_id = node.id;
    out.text = name_of(node);
    out.description = description_of(node);
    out.x = node.bounds.x;
    out.y = node.bounds.y;
    out.width = node.bounds.width;
    out.height = node.bounds.height;
    out.focusable =
        has_state(node.states, State::Focusable) || has_state(node.states, State::Focused);
    // Disabled is the one inverted state: the engine records "disabled", the platform asks
    // "enabled". `android_a11y_mapping`'s `describe` owns the same rule, and this mirrors it so the
    // pull path and the push path cannot disagree.
    out.enabled = !has_state(node.states, State::Disabled);
    out.selected = has_state(node.states, State::Selected);
    out.checkable =
        node.role == Role::Checkbox || node.role == Role::Radio || node.role == Role::Switch;
    out.checked = out.checkable && has_state(node.states, State::Checked);
    out.editable = node.role == Role::TextField;
    out.scrollable = has_action(node.actions, Action::Scroll);
    out.focused = has_state(node.states, State::Focused);
    out.actions = node.actions;
    // A text field's content is its VALUE on the platform and its label is the name; reporting the
    // typed content as the name makes a screen reader announce the content as the control's label.
    if (out.editable) {
        out.description = value_of(node);
        out.text = name_of(node);
    }
    return true;
}

std::size_t SnapshotProvider::children_of(int virtual_id, std::span<int> out) const {
    std::lock_guard lock(mutex_);
    if (out.empty() || node_count_ == 0) {
        return 0;
    }
    std::size_t written = 0;
    if (virtual_id == kHostVirtualId) {
        // The host's children are the snapshot roots. A root is a node no other node lists as a
        // child; building that set per call would scan the whole tree, but the snapshot caps at
        // kMaxNodes and the platform asks for this once per window, not per node.
        bool is_child[kMaxNodes] = {};
        for (std::size_t index = 0; index < node_count_; ++index) {
            const A11yNode& node = nodes_.nodes[index];
            for (std::size_t child = 0; child < node.child_count; ++child) {
                const std::size_t target = node.children[child];
                if (target < node_count_) {
                    is_child[target] = true;
                }
            }
        }
        for (std::size_t index = 0; index < node_count_ && written < out.size(); ++index) {
            if (!is_child[index]) {
                out[written] = virtual_id_of_index_[index];
                ++written;
            }
        }
        return written;
    }
    const std::size_t index = index_of(virtual_id);
    if (index == kMaxNodes) {
        return 0;
    }
    const A11yNode& node = nodes_.nodes[index];
    for (std::size_t child = 0; child < node.child_count && written < out.size(); ++child) {
        const std::size_t target = node.children[child];
        if (target < node_count_) {
            out[written] = virtual_id_of_index_[target];
            ++written;
        }
    }
    return written;
}

bool SnapshotProvider::post_action(int virtual_id, Action action, std::string_view text) {
    if (action == Action::None) {
        return false;
    }
    // The payload is only meaningful for the two value-setting actions, and a request that carries
    // one for anything else is refused rather than silently dropped: the platform offered something
    // the host would not act on, and answering `true` would tell it the request succeeded.
    const bool wants_text = action == Action::SetValue || action == Action::SetSelection;
    if (!wants_text && !text.empty()) {
        return false;
    }
    if (text.size() >= kMaxProviderActionTextBytes) {
        // Refused, not truncated: a truncated field text is worse than a refused edit, because the
        // screen reader would then hold a buffer the engine does not.
        return false;
    }
    std::lock_guard lock(mutex_);
    const std::size_t index = index_of(virtual_id);
    if (index == kMaxNodes) {
        // A stale id from a tree the app already replaced: a normal race, not an error.
        return false;
    }
    if (action_count_ == actions_.size()) {
        // Bounded and drop-oldest: a client that queues faster than the UI thread drains cannot
        // grow this queue or stretch a frame.
        ++dropped_actions_;
        return false;
    }
    ProviderAction& queued = actions_[action_count_];
    queued = ProviderAction{};
    queued.node = nodes_.nodes[index].id;
    queued.action = action;
    if (!text.empty()) {
        std::memcpy(queued.text.data(), text.data(), text.size());
    }
    ++action_count_;
    return true;
}

std::size_t SnapshotProvider::take_actions(std::span<ProviderAction> out) {
    std::lock_guard lock(mutex_);
    const std::size_t taken = std::min(out.size(), action_count_);
    for (std::size_t i = 0; i < taken; ++i) {
        out[i] = actions_[i];
    }
    // Shift the remainder down rather than keeping a ring: kMaxProviderActions is 32, so this is a
    // bounded copy of at most 32 structs on a tick that actually had a request.
    for (std::size_t i = taken; i < action_count_; ++i) {
        actions_[i - taken] = actions_[i];
    }
    action_count_ -= taken;
    return taken;
}

} // namespace tessera::ui::a11y
