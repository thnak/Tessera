// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/atspi_events.hpp"

#include <algorithm>
#include <cstring>

namespace tessera::ui::a11y {

void set_event_text(std::array<char, kMaxEventTextBytes>& target, std::string_view text) {
    if (text.size() >= kMaxEventTextBytes) {
        std::size_t end = kMaxEventTextBytes - 1;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
            --end;
        }
        text = text.substr(0, end);
    }
    target.fill('\0');
    std::copy(text.begin(), text.end(), target.begin());
}

std::string_view event_detail(const A11yEvent& event) {
    return std::string_view(event.detail.data());
}

std::string_view event_value(const A11yEvent& event) {
    return std::string_view(event.value.data());
}

A11yEvent make_state_event(A11yIndex node, std::string_view state, bool enabled) {
    A11yEvent event;
    event.kind = EventKind::StateChanged;
    event.node = node;
    event.detail1 = enabled ? 1 : 0;
    set_event_text(event.detail, state);
    return event;
}

A11yEvent make_focus_event(A11yIndex node) {
    return make_state_event(node, "focused", true);
}

A11yEvent make_property_event(A11yIndex node, std::string_view property, std::string_view value) {
    A11yEvent event;
    event.kind = EventKind::PropertyChanged;
    event.node = node;
    set_event_text(event.detail, property);
    set_event_text(event.value, value);
    return event;
}

A11yEvent make_bounds_event(A11yIndex node) {
    A11yEvent event;
    event.kind = EventKind::BoundsChanged;
    event.node = node;
    return event;
}

A11yEvent make_visible_data_event(A11yIndex node) {
    A11yEvent event;
    event.kind = EventKind::VisibleDataChanged;
    event.node = node;
    return event;
}

std::string_view event_signal_member(EventKind kind) {
    switch (kind) {
    case EventKind::StateChanged:
        return "StateChanged";
    case EventKind::PropertyChanged:
        return "PropertyChange";
    case EventKind::BoundsChanged:
        return "BoundsChanged";
    case EventKind::VisibleDataChanged:
        return "VisibleDataChanged";
    }
    return "StateChanged";
}

bool EventQueue::push(const A11yEvent& event) {
    std::lock_guard lock(mutex_);
    // Coalescing scans at most kMaxQueuedEvents entries; the queue is a fixed 64-slot ring
    // precisely so this stays cheap on the posting thread.
    for (std::size_t offset = 0; offset < count_; ++offset) {
        A11yEvent& queued = events_[(head_ + offset) % kMaxQueuedEvents];
        if (queued.kind == event.kind && queued.node == event.node &&
            event_detail(queued) == event_detail(event)) {
            queued.detail1 = event.detail1;
            queued.detail2 = event.detail2;
            std::memcpy(queued.value.data(), event.value.data(), kMaxEventTextBytes);
            ++coalesced_;
            return true;
        }
    }
    if (count_ >= kMaxQueuedEvents) {
        ++dropped_;
        return false;
    }
    events_[(head_ + count_) % kMaxQueuedEvents] = event;
    ++count_;
    return true;
}

std::size_t EventQueue::take(std::span<A11yEvent> out) {
    std::lock_guard lock(mutex_);
    const std::size_t taken = std::min(out.size(), count_);
    for (std::size_t index = 0; index < taken; ++index) {
        out[index] = events_[(head_ + index) % kMaxQueuedEvents];
    }
    head_ = (head_ + taken) % kMaxQueuedEvents;
    count_ -= taken;
    return taken;
}

std::size_t EventQueue::size() const {
    std::lock_guard lock(mutex_);
    return count_;
}

std::size_t EventQueue::dropped() const {
    std::lock_guard lock(mutex_);
    return dropped_;
}

std::size_t EventQueue::coalesced() const {
    std::lock_guard lock(mutex_);
    return coalesced_;
}

void EventQueue::clear() {
    std::lock_guard lock(mutex_);
    head_ = 0;
    count_ = 0;
}

} // namespace tessera::ui::a11y
