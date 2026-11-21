// SPDX-License-Identifier: MIT
//
// The announcement queue behind `useAnnounce` (ADR-064 item 7, issue #629): the bounded,
// de-duplicating list the accessibility bridge drains.
//
// The queue is its own structure rather than a node, because an announcement has no box, is never
// painted, and must survive the component that produced it. These tests pin the ordering, the
// de-duplication and the caps.

#include "tessera/ui/accessibility/announcement_queue.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::ui::accessibility::Announcement;
using tessera::ui::accessibility::AnnouncementQueue;
using tessera::ui::accessibility::LiveChannel;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "announcement_queue: " << message << "\n";
        ++failures;
    }
}

void test_polite_is_queued_and_drained() {
    AnnouncementQueue queue;
    expect(queue.announce(LiveChannel::Polite, "Saved"), "a polite message is queued");
    expect(queue.size() == 1, "the queue holds one");
    Announcement out;
    expect(queue.take_next(out), "the message drains");
    expect(out.text == "Saved", "the text is carried");
    expect(out.channel == LiveChannel::Polite, "the channel is carried");
    expect(queue.empty(), "the queue is empty after the drain");
}

void test_assertive_goes_first() {
    AnnouncementQueue queue;
    expect(queue.announce(LiveChannel::Polite, "Saved"), "a polite message is queued");
    expect(queue.announce(LiveChannel::Assertive, "Error"), "an assertive message is queued");
    Announcement out;
    expect(queue.take_next(out), "the first message drains");
    expect(out.text == "Error", "the assertive message interrupts");
    expect(queue.take_next(out), "the second message drains");
    expect(out.text == "Saved", "the polite message follows");
}

void test_duplicate_is_suppressed() {
    AnnouncementQueue queue;
    expect(queue.announce(LiveChannel::Polite, "Saved"), "the first message is queued");
    expect(!queue.announce(LiveChannel::Polite, "Saved"), "the same message is suppressed");
    expect(queue.size() == 1, "the queue still holds one");
    // A different channel is a different announcement.
    expect(queue.announce(LiveChannel::Assertive, "Saved"),
           "the same text on another channel is new");
}

// The de-duplication survives a drain: a component that re-announces after the queue emptied is
// still repeating itself.
void test_duplicate_survives_a_drain() {
    AnnouncementQueue queue;
    expect(queue.announce(LiveChannel::Polite, "Saved"), "the first message is queued");
    Announcement out;
    expect(queue.take_next(out), "the message drains");
    expect(!queue.announce(LiveChannel::Polite, "Saved"),
           "the same message after a drain is still suppressed");
    expect(queue.last_text(LiveChannel::Polite) == "Saved", "the last text is remembered");
}

void test_off_and_empty_are_refused() {
    AnnouncementQueue queue;
    expect(!queue.announce(LiveChannel::Off, "Saved"), "the off channel refuses");
    expect(!queue.announce(LiveChannel::Polite, ""), "an empty message is refused");
    expect(queue.empty(), "nothing was queued");
}

void test_text_is_truncated() {
    AnnouncementQueue queue;
    const std::string long_text(AnnouncementQueue::kMaxTextBytes + 100, 'x');
    expect(queue.announce(LiveChannel::Polite, long_text), "a long message is accepted");
    Announcement out;
    expect(queue.take_next(out), "the message drains");
    expect(out.text.size() == AnnouncementQueue::kMaxTextBytes, "the text is truncated to the cap");
}

void test_queue_is_bounded() {
    AnnouncementQueue queue;
    for (std::size_t i = 0; i < AnnouncementQueue::kMaxQueued; ++i) {
        expect(queue.announce(LiveChannel::Polite, "message " + std::to_string(i)),
               "a message is queued");
    }
    expect(queue.size() == AnnouncementQueue::kMaxQueued, "the queue is full");
    // One more drops the oldest polite message rather than growing.
    expect(queue.announce(LiveChannel::Polite, "overflow"), "the overflow message is queued");
    expect(queue.size() == AnnouncementQueue::kMaxQueued, "the queue stays bounded");
    Announcement out;
    expect(queue.take_next(out), "the first message drains");
    expect(out.text == "message 1", "the oldest polite message was dropped, not the newest");
}

void test_scope_clear() {
    AnnouncementQueue queue;
    expect(queue.announce(LiveChannel::Polite, "dialog", 7), "a scoped message is queued");
    expect(queue.announce(LiveChannel::Polite, "page", 0), "an unscoped message is queued");
    queue.clear_scope(7);
    expect(queue.size() == 1, "only the scoped message is dropped");
    Announcement out;
    expect(queue.take_next(out), "the remaining message drains");
    expect(out.text == "page", "the unscoped message survives");
}

void test_clear() {
    AnnouncementQueue queue;
    expect(queue.announce(LiveChannel::Polite, "one"), "a message is queued");
    expect(queue.announce(LiveChannel::Assertive, "two"), "a second message is queued");
    queue.clear();
    expect(queue.empty(), "clear drops everything");
    expect(!queue.announce(LiveChannel::Polite, "one"),
           "the last-text record survives a clear, so a repeat is still suppressed");
}

void test_peek_does_not_remove() {
    AnnouncementQueue queue;
    expect(queue.announce(LiveChannel::Polite, "Saved"), "a message is queued");
    const Announcement* peeked = queue.peek();
    expect(peeked != nullptr && peeked->text == "Saved", "peek returns the next message");
    expect(queue.size() == 1, "peek does not remove it");
    expect(queue.peek() == nullptr || queue.size() == 1, "peek is stable");
}

} // namespace

int main() {
    test_polite_is_queued_and_drained();
    test_assertive_goes_first();
    test_duplicate_is_suppressed();
    test_duplicate_survives_a_drain();
    test_off_and_empty_are_refused();
    test_text_is_truncated();
    test_queue_is_bounded();
    test_scope_clear();
    test_clear();
    test_peek_does_not_remove();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
