// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/uia_events.hpp"
#include "tessera/ui/accessibility/uia_mapping.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

namespace tessera::ui::a11y {
namespace {

constexpr std::size_t kNotFound = std::numeric_limits<std::size_t>::max();

bool text_is(std::string_view text, std::string_view expected) {
    return text == expected;
}

std::string_view node_name(const A11yNode& node) {
    return name_of(node);
}

std::string_view node_value(const A11yNode& node) {
    return value_of(node);
}

std::string_view node_description(const A11yNode& node) {
    return description_of(node);
}

std::string_view bool_text(bool value) {
    return value ? "1" : "0";
}

bool bounds_equal(const rendering::rhi::Rect& lhs, const rendering::rhi::Rect& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width && lhs.height == rhs.height;
}

bool children_equal(const A11yNode& lhs, const A11yNode& rhs) {
    if (lhs.child_count != rhs.child_count) {
        return false;
    }
    return std::equal(lhs.children.begin(), lhs.children.begin() + lhs.child_count,
                      rhs.children.begin());
}

// Same children, different order: UIA can express that as Reordered instead of Invalidated.
bool children_same_set(const A11yNode& lhs, const A11yNode& rhs) {
    if (lhs.child_count != rhs.child_count) {
        return false;
    }
    for (std::uint16_t index = 0; index < lhs.child_count; ++index) {
        const A11yIndex child = lhs.children[index];
        const bool found = std::find(rhs.children.begin(), rhs.children.begin() + rhs.child_count,
                                     child) != rhs.children.begin() + rhs.child_count;
        if (!found) {
            return false;
        }
    }
    return true;
}

const A11yNode* previous_node(const A11ySnapshot& previous, NodeId id) {
    for (const A11yNode& node : previous.nodes) {
        if (node.id == id) {
            return &node;
        }
    }
    return nullptr;
}

A11yIndex focused_index(const A11ySnapshot& snapshot) {
    for (std::size_t index = 0; index < snapshot.nodes.size(); ++index) {
        if (has_state(snapshot.nodes[index].states, State::Focused)) {
            return static_cast<A11yIndex>(index);
        }
    }
    return kInvalidIndex;
}

bool same_event_key(const UiaEvent& lhs, const UiaEvent& rhs) {
    if (lhs.kind != rhs.kind) {
        return false;
    }
    if (lhs.kind == UiaEventKind::FocusChanged) {
        return true; // one pending focus event, newest wins
    }
    if (lhs.node != rhs.node) {
        return false;
    }
    // StructureChanged coalesces per node; PropertyChanged per (node, property).
    return lhs.kind == UiaEventKind::StructureChanged || lhs.property == rhs.property;
}

} // namespace

UiaEvent make_uia_structure_event(A11yIndex node, std::int32_t change_type) {
    UiaEvent event;
    event.kind = UiaEventKind::StructureChanged;
    event.node = node;
    event.property = change_type;
    return event;
}

UiaEvent make_uia_property_event(A11yIndex node, std::int32_t property, std::string_view value) {
    UiaEvent event;
    event.kind = UiaEventKind::PropertyChanged;
    event.node = node;
    event.property = property;
    set_event_text(event.value, value);
    return event;
}

UiaEvent make_uia_focus_event(A11yIndex node) {
    UiaEvent event;
    event.kind = UiaEventKind::FocusChanged;
    event.node = node;
    return event;
}

std::string_view uia_event_value(const UiaEvent& event) {
    return std::string_view(event.value.data());
}

std::size_t UiaEventQueue::find_locked(const UiaEvent& event) const {
    for (std::size_t offset = 0; offset < count_; ++offset) {
        if (same_event_key(events_[(head_ + offset) % events_.size()], event)) {
            return offset;
        }
    }
    return kNotFound;
}

void UiaEventQueue::append_locked(const UiaEvent& event) {
    events_[(head_ + count_) % events_.size()] = event;
    ++count_;
}

bool UiaEventQueue::push(const UiaEvent& event) {
    std::lock_guard lock(mutex_);
    const std::size_t found = find_locked(event);
    if (found != kNotFound) {
        UiaEvent& slot = events_[(head_ + found) % events_.size()];
        if (event.kind == UiaEventKind::PropertyChanged &&
            text_is(uia_event_value(slot), uia_event_value(event))) {
            ++deduped_; // the same value is still pending: nothing new to tell the client
            return true;
        }
        ++coalesced_;
        slot = event;
        return true;
    }
    if (count_ == events_.size()) {
        // Drop-oldest: the newest notification is the one that describes the current tree.
        head_ = (head_ + 1) % events_.size();
        --count_;
        ++dropped_;
        append_locked(event);
        return false;
    }
    append_locked(event);
    return true;
}

std::size_t UiaEventQueue::take(std::span<UiaEvent> out) {
    std::lock_guard lock(mutex_);
    const std::size_t taken = std::min(out.size(), count_);
    for (std::size_t offset = 0; offset < taken; ++offset) {
        out[offset] = events_[(head_ + offset) % events_.size()];
    }
    head_ = (head_ + taken) % events_.size();
    count_ -= taken;
    return taken;
}

std::size_t UiaEventQueue::size() const {
    std::lock_guard lock(mutex_);
    return count_;
}

std::size_t UiaEventQueue::dropped() const {
    std::lock_guard lock(mutex_);
    return dropped_;
}

std::size_t UiaEventQueue::coalesced() const {
    std::lock_guard lock(mutex_);
    return coalesced_;
}

std::size_t UiaEventQueue::deduped() const {
    std::lock_guard lock(mutex_);
    return deduped_;
}

void UiaEventQueue::clear() {
    std::lock_guard lock(mutex_);
    head_ = 0;
    count_ = 0;
}

UiaEventDiff queue_snapshot_diff(const A11ySnapshot& previous, const A11ySnapshot& next,
                                 UiaEventQueue& queue) {
    UiaEventDiff diff;
    for (const A11yNode& node : next.nodes) {
        if (previous_node(previous, node.id) == nullptr) {
            diff.nodes_added = true;
            break;
        }
    }
    for (const A11yNode& node : previous.nodes) {
        bool found = false;
        for (const A11yNode& candidate : next.nodes) {
            if (candidate.id == node.id) {
                found = true;
                break;
            }
        }
        if (!found) {
            diff.nodes_removed = true;
            break;
        }
    }

    // One root invalidation covers additions, removals and re-parenting: the client re-queries the
    // whole tree instead of receiving one event per moved node.
    if (previous.size() != next.size() || diff.nodes_added || diff.nodes_removed) {
        const A11yIndex root = next.nodes.empty() ? kInvalidIndex : static_cast<A11yIndex>(0);
        if (queue.push(make_uia_structure_event(root, kUiaChildrenInvalidated))) {
            ++diff.queued;
        }
        ++diff.structure;
    }

    for (std::size_t index = 0; index < next.nodes.size(); ++index) {
        const A11yNode& current = next.nodes[index];
        const A11yNode* before = previous_node(previous, current.id);
        if (before == nullptr) {
            continue; // announced by the root invalidation above
        }
        const auto node = static_cast<A11yIndex>(index);
        if (!children_equal(*before, current)) {
            const std::int32_t change = children_same_set(*before, current)
                                            ? kUiaChildrenReordered
                                            : kUiaChildrenInvalidated;
            if (queue.push(make_uia_structure_event(node, change))) {
                ++diff.queued;
            }
            ++diff.structure;
        }
        // Property order is fixed so the raised sequence is deterministic.
        if (!text_is(node_name(*before), node_name(current))) {
            if (queue.push(make_uia_property_event(node, kUiaNamePropertyId, node_name(current)))) {
                ++diff.queued;
            }
            ++diff.properties;
        }
        if (!text_is(node_value(*before), node_value(current))) {
            if (queue.push(
                    make_uia_property_event(node, kUiaValueValuePropertyId, node_value(current)))) {
                ++diff.queued;
            }
            ++diff.properties;
        }
        if (!text_is(node_description(*before), node_description(current))) {
            if (queue.push(make_uia_property_event(node, kUiaHelpTextPropertyId,
                                                   node_description(current)))) {
                ++diff.queued;
            }
            ++diff.properties;
        }
        const bool before_enabled = !has_state(before->states, State::Disabled);
        const bool now_enabled = !has_state(current.states, State::Disabled);
        if (before_enabled != now_enabled) {
            if (queue.push(make_uia_property_event(node, kUiaIsEnabledPropertyId,
                                                   bool_text(now_enabled)))) {
                ++diff.queued;
            }
            ++diff.properties;
        }
        const bool before_selected = has_state(before->states, State::Selected);
        const bool now_selected = has_state(current.states, State::Selected);
        if (before_selected != now_selected) {
            if (queue.push(make_uia_property_event(node, kUiaSelectionItemIsSelectedPropertyId,
                                                   bool_text(now_selected)))) {
                ++diff.queued;
            }
            ++diff.properties;
        }
        if (!bounds_equal(before->bounds, current.bounds)) {
            if (queue.push(make_uia_property_event(node, kUiaBoundingRectanglePropertyId))) {
                ++diff.queued;
            }
            ++diff.properties;
        }
    }

    const A11yIndex before_focus = focused_index(previous);
    const A11yIndex after_focus = focused_index(next);
    const NodeId before_id = before_focus == kInvalidIndex ? 0 : previous.nodes[before_focus].id;
    const NodeId after_id = after_focus == kInvalidIndex ? 0 : next.nodes[after_focus].id;
    if (after_focus != kInvalidIndex && before_id != after_id) {
        if (queue.push(make_uia_focus_event(after_focus))) {
            ++diff.queued;
        }
        diff.focus_changed = true;
    }
    return diff;
}

} // namespace tessera::ui::a11y
