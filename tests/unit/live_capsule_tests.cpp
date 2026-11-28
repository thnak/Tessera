// SPDX-License-Identifier: MIT
//
// LiveCapsule (issue #224, docs/specs/live-capsule-v1.md section 9): placement from fake cutouts,
// sizing and state transitions, morph (spring and reduced motion), queue ordering and preemption,
// timeouts, in-place update and the announcements.

#include "tessera/hal/window_cutout_fold.hpp"
#include "tessera/ui/capsule/capsule_morph.hpp"
#include "tessera/ui/capsule/capsule_placement.hpp"
#include "tessera/ui/capsule/capsule_state.hpp"
#include "tessera/ui/capsule/live_capsule_host.hpp"

#include <chrono>
#include <cmath>
#include <iostream>

namespace {

using namespace tessera::ui::capsule;
using tessera::hal::CutoutInfo;
using tessera::hal::WindowRect;
using tessera::ui::accessibility::Announcement;
using tessera::ui::accessibility::AnnouncementQueue;
using namespace std::chrono_literals;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "live_capsule: " << message << "\n";
        ++failures;
    }
    return condition;
}

bool near(float a, float b) {
    return std::fabs(a - b) < 0.01f;
}

CapsulePlacementInput portrait() {
    CapsulePlacementInput input;
    input.window_width = 400.0f;
    input.window_height = 800.0f;
    input.top_inset = 24.0f;
    input.width = 160.0f;
    input.height = 36.0f;
    return input;
}

void add_cutout(CutoutInfo& info, WindowRect rect) {
    info.bounds[info.count++] = rect;
}

void test_placement() {
    auto input = portrait();
    auto none = place_capsule(input);
    expect(!none.on_cutout && near(none.rect.x, 120.0f) && near(none.rect.y, 32.0f),
           "no cutout: top centre, 8 below the inset");

    add_cutout(input.cutouts, {170.0f, 0.0f, 60.0f, 30.0f});
    auto notch = place_capsule(input);
    expect(notch.on_cutout && near(notch.rect.x + notch.rect.width * 0.5f, 200.0f),
           "one cutout: centred on it horizontally");
    expect(near(notch.rect.y, 0.0f), "a cutout taller than half the box keeps the box at the top");

    add_cutout(input.cutouts, {50.0f, 0.0f, 100.0f, 20.0f});
    auto wide = place_capsule(input);
    expect(near(wide.rect.x + wide.rect.width * 0.5f, 100.0f), "several top cutouts: the widest");

    auto landscape = portrait();
    add_cutout(landscape.cutouts, {0.0f, 300.0f, 30.0f, 60.0f});
    expect(!place_capsule(landscape).on_cutout, "a side cutout falls back to the top-centre rule");
    CutoutInfo corner;
    add_cutout(corner, {0.0f, 0.0f, 40.0f, 40.0f});
    landscape.cutouts = corner;
    expect(!place_capsule(landscape).on_cutout, "a corner cutout touching a side edge is skipped");

    auto punch = portrait();
    add_cutout(punch.cutouts, {190.0f, 10.0f, 20.0f, 20.0f});
    expect(!place_capsule(punch).on_cutout, "a hole not touching the top edge is not a top cutout");

    auto hinge = portrait();
    hinge.hinge = {190.0f, 0.0f, 20.0f, 800.0f};
    hinge.width = 120.0f;
    auto moved = place_capsule(hinge);
    expect(moved.moved_for_hinge, "a hinge under the capsule moves it");
    expect(moved.rect.x + moved.rect.width <= 190.0f || moved.rect.x >= 210.0f,
           "the capsule clears the hinge");
    hinge.hinge = {150.0f, 0.0f, 20.0f, 800.0f};
    auto pane = place_capsule(hinge);
    expect(pane.rect.x >= 170.0f && near(pane.rect.x + pane.rect.width * 0.5f, 285.0f),
           "the larger pane is the right one");

    auto hostile = portrait();
    hostile.window_width = std::nanf("");
    hostile.width = -5.0f;
    const auto safe = place_capsule(hostile);
    expect(std::isfinite(safe.rect.x) && safe.rect.width == 0.0f, "hostile input stays finite");
}

void test_sizing_and_transitions() {
    CapsuleSizeInput in;
    in.window_width = 400.0f;
    in.window_height = 800.0f;
    in.cutout_width = 60.0f;
    in.cutout_height = 30.0f;
    in.leading_width = 80.0f;
    in.trailing_width = 80.0f;
    in.card_width = 500.0f;
    in.card_height = 600.0f;

    in.state = CapsuleState::Minimal;
    expect(near(capsule_size(in).width, 60.0f + kMinimalDotWidth), "minimal: cutout plus one dot");
    in.state = CapsuleState::Compact;
    expect(near(capsule_size(in).width, 220.0f), "compact: cutout plus slots");
    in.leading_width = 400.0f;
    expect(near(capsule_size(in).width, 240.0f), "compact is capped at 60% of the window");
    in.leading_width = 20.0f;
    in.trailing_width = 50.0f;
    expect(near(capsule_size(in).width, 60.0f + 2.0f * 50.0f),
           "compact: the wider slot sets both sides so neither runs under the cutout");
    in.state = CapsuleState::Expanded;
    const auto card = capsule_size(in);
    expect(near(card.width, 360.0f) && near(card.height, 320.0f),
           "expanded is capped at 90% x 40%");
    in.state = CapsuleState::Compact;
    in.cutout_width = 0.0f;
    in.leading_width = 50.0f;
    in.trailing_width = 30.0f;
    expect(near(capsule_size(in).width, 80.0f), "no cutout: a single row");

    CapsuleController c;
    c.reset(CapsuleState::Compact);
    expect(!c.escape() && c.state() == CapsuleState::Compact, "escape on compact does nothing");
    expect(c.activate(42) && c.state() == CapsuleState::Expanded, "tap expands");
    expect(!c.activate(7), "a second tap on expanded does nothing");
    expect(c.swipe_up(2.0f, 20.0f) == false && c.state() == CapsuleState::Expanded,
           "a swipe starting in the gesture inset is ignored");
    expect(c.swipe_up(100.0f, 20.0f) && c.state() == CapsuleState::Compact,
           "an upward swipe collapses to the previous state");
    expect(c.take_focus_return() == 42 && c.take_focus_return() == 0, "focus return, once");
    c.reset(CapsuleState::Minimal);
    c.activate(0);
    expect(c.outside_press() && c.state() == CapsuleState::Minimal,
           "outside press returns to minimal");
    c.activate(0);
    expect(c.escape() && c.state() == CapsuleState::Minimal, "escape collapses");
}

void test_morph() {
    CapsuleMorph morph;
    morph.snap({100.0f, 36.0f, 18.0f});
    expect(!morph.active(), "snap is idle");
    morph.set_target({300.0f, 200.0f, 20.0f});
    expect(morph.active(), "a retarget wakes the pacer");
    int frames = 0;
    while (morph.tick(1.0f / 60.0f) && ++frames < 1000) {
    }
    expect(frames < 1000 && !morph.active(), "the spring settles");
    expect(near(morph.current().width, 300.0f) && near(morph.current().height, 200.0f),
           "settled at the target");

    CapsuleMorph reduced;
    reduced.set_reduced_motion(true);
    reduced.snap({100.0f, 36.0f, 18.0f});
    reduced.set_target({300.0f, 200.0f, 20.0f});
    expect(near(reduced.current().width, 300.0f), "reduced motion: size snaps");
    expect(reduced.cross_fade() == 0.0f && reduced.active(), "reduced motion: cross-fade starts");
    reduced.tick(0.075f);
    expect(near(reduced.cross_fade(), 0.5f), "cross-fade is linear");
    reduced.tick(0.1f);
    expect(reduced.cross_fade() == 1.0f && !reduced.active(), "cross-fade ends idle");
}

Activity activity(const char* id, Priority priority = Priority::Normal,
                  ActivityForm form = ActivityForm::Compact) {
    Activity a;
    a.id = id;
    a.label = id;
    a.priority = priority;
    a.form = form;
    return a;
}

void test_host_queue() {
    LiveCapsuleHost host;
    expect(host.front() == nullptr && !host.next_deadline(), "empty host");
    expect(!host.start(activity("")), "an empty id is rejected");
    host.start(activity("timer"));
    host.start(activity("transfer"));
    expect(host.front()->id == "timer", "arrival order within a priority");
    const auto gen = host.front_generation();
    host.start(activity("call", Priority::High));
    expect(host.front()->id == "call" && host.front_generation() > gen, "high preempts");
    expect((host.ordered_ids() == std::vector<std::string>{"call", "timer", "transfer"}),
           "the preempted activity goes back to the queue, ordered");
    host.start(activity("low", Priority::Low));
    host.start(activity("dot", Priority::Normal, ActivityForm::Minimal));
    expect(host.minimal()->id == "dot" && host.front()->id == "call",
           "one compact plus one minimal are visible");
    expect(
        (host.ordered_ids() == std::vector<std::string>{"call", "timer", "transfer", "low", "dot"}),
        "order: compact by priority then arrival, then minimal");
    expect(host.end("call") && host.front()->id == "timer", "the next one takes the front");
    expect(!host.end("call"), "ending twice is a no-op");
}

void test_host_update_timeout_announce() {
    AnnouncementQueue queue;
    LiveCapsuleHost host(&queue);
    Activity a = activity("rec");
    a.slots.title = "one";
    a.timeout = 5s;
    host.start(std::move(a));
    expect(queue.size() == 1 &&
               queue.peek()->channel == tessera::ui::accessibility::LiveChannel::Polite,
           "start announces once, politely");

    Slots slots;
    slots.title = "two";
    const auto revision = host.revision();
    expect(host.update("rec", std::move(slots)) && host.find("rec")->slots.title == "two",
           "update replaces slots in place");
    expect(host.revision() > revision && queue.size() == 1,
           "update bumps the revision, never announces");
    expect(!host.update("nope", {}), "update of an unknown id");

    Activity again = activity("rec");
    again.slots.title = "three";
    host.start(std::move(again));
    expect(host.size() == 1 && host.find("rec")->slots.title == "three" && queue.size() == 1,
           "start with a live id updates in place, no second announcement");

    expect(host.next_deadline() == std::optional<Duration>(5s), "deadline from the host clock");
    expect(host.tick(4s) == 0 && host.size() == 1, "not yet");
    expect(host.tick(5s) == 1 && host.size() == 0, "timeout ends the activity");
    Announcement out;
    queue.take_next(out);
    expect(queue.take_next(out) && out.text == "rec ended", "the timeout announces the end");

    Activity late = activity("late");
    late.timeout = 2s;
    host.start(std::move(late));
    expect(host.next_deadline() == std::optional<Duration>(7s), "a later start counts from now");

    Activity custom = activity("x");
    custom.end_message = "Done";
    host.start(std::move(custom));
    host.end("x");
    queue.clear();
    host.end("late");
    expect(queue.size() == 1, "an explicit end announces once");

    Slots many;
    many.actions.resize(5);
    host.start(activity("acts"));
    host.update("acts", std::move(many));
    expect(host.find("acts")->slots.actions.size() == kMaxActions, "at most two actions");
}

} // namespace

int main() {
    test_placement();
    test_sizing_and_transitions();
    test_morph();
    test_host_queue();
    test_host_update_timeout_announce();
    if (failures != 0) {
        std::cerr << "live_capsule: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "live_capsule: ok\n";
    return 0;
}
