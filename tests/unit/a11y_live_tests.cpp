// SPDX-License-Identifier: MIT
//
// Live regions in the accessibility tree builder (issue #55 follow-up, accessibility-v1.md section
// 8): `aria-live` and the implicit alert/status behaviour resolve onto A11yNode::live, and a
// change of a live region's text between snapshots becomes an announcement.

#include "a11y_test_helpers.hpp"
#include "tessera/ui/accessibility/atspi_bridge.hpp"
#include "tessera/ui/accessibility/live_regions.hpp"

using namespace a11y_test;
using tessera::ui::a11y::collect_live_announcements;
using tessera::ui::a11y::Live;
using tessera::ui::accessibility::Announcement;
using tessera::ui::accessibility::AnnouncementQueue;
using tessera::ui::accessibility::LiveChannel;

namespace {

bool resolution() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    Node& plain = child(root, 2);
    plain.set_element_kind("Text");
    Node& alert = child(root, 3);
    alert.set_role("alert");
    Node& status = child(root, 4);
    status.set_role("status");
    Node& toast = child(root, 5);
    toast.set_role("alert");
    toast.set_aria("aria-live", "polite");
    Node& assertive = child(root, 6);
    assertive.set_aria("aria-live", "assertive");
    Node& off = child(root, 7);
    off.set_role("status");
    off.set_aria("aria-live", "off");
    Node& bogus = child(root, 8);
    bogus.set_aria("aria-live", "loud");

    StubStyles styles;
    A11ySnapshot snapshot;
    (void)build(root, styles, snapshot);
    return expect(snapshot.nodes.size() == 8, "all nodes emitted") &&
           expect(snapshot.nodes[0].live == Live::Off, "root is not live") &&
           expect(snapshot.nodes[1].live == Live::Off, "plain text is not live") &&
           expect(snapshot.nodes[2].live == Live::Assertive, "alert is implicitly assertive") &&
           expect(snapshot.nodes[3].live == Live::Polite, "status is implicitly polite") &&
           expect(snapshot.nodes[4].live == Live::Polite, "explicit polite wins over alert") &&
           expect(snapshot.nodes[5].live == Live::Assertive, "aria-live assertive on a generic") &&
           expect(snapshot.nodes[6].live == Live::Off, "explicit off wins over status") &&
           expect(snapshot.nodes[7].live == Live::Off, "an unknown value is ignored");
}

bool announcements() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    Node& region = child(root, 2);
    region.set_role("status");
    region.set_aria("aria-label", "Saved");
    StubStyles styles;
    A11ySnapshot first;
    A11ySnapshot second;
    A11ySnapshot third;
    A11ySnapshot empty;
    (void)build(root, styles, first);

    AnnouncementQueue queue;
    if (!expect(collect_live_announcements(empty, first, queue) == 0,
                "the initial snapshot announces nothing")) {
        return false;
    }
    (void)build(root, styles, second);
    if (!expect(collect_live_announcements(first, second, queue) == 0,
                "an unchanged region announces nothing")) {
        return false;
    }

    region.set_aria("aria-label", "Saved 2");
    Node& toast = child(root, 3);
    toast.set_role("alert");
    toast.set_aria("aria-live", "polite");
    toast.set_aria("aria-label", "Upload finished");
    (void)build(root, styles, third);
    if (!expect(collect_live_announcements(second, third, queue) == 2,
                "a changed region and a new toast announce")) {
        return false;
    }
    Announcement one;
    Announcement two;
    return expect(queue.take_next(one) && queue.take_next(two), "two queued") &&
           expect(one.text == "Saved 2" && one.channel == LiveChannel::Polite, "status text") &&
           expect(two.text == "Upload finished" && two.channel == LiveChannel::Polite,
                  "the toast announces politely");
}

// Publishing a snapshot where a toast appeared queues one announcement event; an identical
// republish queues nothing more.
bool bridge_publish_announces() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    StubStyles styles;
    A11ySnapshot before;
    (void)build(root, styles, before);
    Node& toast = child(root, 2);
    toast.set_role("alert");
    toast.set_aria("aria-live", "polite");
    toast.set_aria("aria-label", "Saved");
    A11ySnapshot after;
    (void)build(root, styles, after);

    tessera::ui::a11y::AtspiBridge bridge;
    bridge.publish(before);
    if (!expect(bridge.pending_events() == 0, "the first publish announces nothing")) {
        return false;
    }
    bridge.publish(after);
    const std::size_t queued = bridge.pending_events();
    bridge.publish(after);
    return expect(queued == 1, "the new toast queues one announcement") &&
           expect(bridge.pending_events() == queued, "a republish adds nothing");
}

} // namespace

int main() {
    return (resolution() && announcements() && bridge_publish_announces()) ? 0 : 1;
}
