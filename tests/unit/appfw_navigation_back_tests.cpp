// SPDX-License-Identifier: MIT
//
// Acceptance word 2: **back** (docs/specs/navigation-v1.md sections 2, 6 and 10.1; issue #196).
//
// Pop semantics, the modal/sheet priority, `pop_to`, `reset`, and the case the issue names
// explicitly: back with an empty stack. Every one of those is defined behaviour here, reported as a
// value.

#include "appfw_navigation_test_support.hpp"

#include <cstdint>
#include <string>

namespace nav_test {
namespace {

void back_pops_the_top_entry() {
    Harness harness;
    (void)harness.controller.push("/orders");
    (void)harness.controller.push("/orders/42");
    expect(harness.controller.depth() == 2, "two entries before back");

    const BackResult result = harness.controller.back();
    expect(result.outcome == BackOutcome::Popped, "back pops a normal entry");
    expect(harness.controller.depth() == 1, "the stack shrank");
    expect(harness.controller.active()->route_id == "orders", "the previous entry is active again");
    expect(result.active_entry == harness.controller.active()->entry_id,
           "the result names the entry that is now active");
}

void back_with_an_empty_stack_is_defined_behaviour() {
    // The issue's own case: a controller that was never given a route. There is no underflow, no
    // crash and no error code to interpret - `Empty` is a documented outcome and the stack stays at
    // zero.
    Harness harness;
    expect(harness.controller.depth() == 0, "the stack starts empty");
    const BackResult empty = harness.controller.back();
    expect(empty.outcome == BackOutcome::Empty, "back on an empty stack is Empty");
    expect(empty.active_entry == 0, "no entry is reported active");
    expect(harness.controller.depth() == 0, "and the stack is still empty");

    // Repeatedly: the same answer, still nothing. A back handler that runs on a key repeat must not
    // accumulate state.
    for (int index = 0; index < 5; ++index) {
        expect(harness.controller.back().outcome == BackOutcome::Empty,
               "back on an empty stack stays Empty");
    }
    expect(harness.controller.depth() == 0, "and never grows or underflows");
    expect(!harness.controller.pending(), "and marks nothing pending");
}

void back_at_root_reports_the_edge_and_does_not_quit() {
    Harness harness;
    (void)harness.controller.push("/");
    expect(harness.controller.depth() == 1, "one entry at the root");

    // Section 2: "Back at root: exit/close per platform policy; never silently quit without
    // confirmation when unsaved work exists." The module reports the edge; the host owns closing a
    // window.
    const BackResult at_root = harness.controller.back();
    expect(at_root.outcome == BackOutcome::AtRoot, "back at the root is AtRoot");
    expect(harness.controller.depth() == 1, "the root entry is not popped");
    expect(harness.controller.active()->route_id == "home", "and it is still active");
    expect(at_root.active_entry == harness.controller.active()->entry_id,
           "the result still names the entry the host's policy applies to");

    // A second back at the root: the same answer. The host decides what a repeat means.
    expect(harness.controller.back().outcome == BackOutcome::AtRoot, "and it stays AtRoot");
}

void a_modal_or_sheet_is_dismissed_before_the_route_below_it() {
    // Section 6: "A modal route is a stack entry with `presentation: modal|sheet`"; section 10.1's
    // back priority puts the topmost modal/sheet above a normal pop.
    Harness harness;
    (void)harness.controller.push("/orders");
    (void)harness.controller.push("/compose");
    expect(harness.controller.active()->presentation == Presentation::Modal,
           "the modal route carries its presentation");

    const BackResult dismissed = harness.controller.back();
    expect(dismissed.outcome == BackOutcome::DismissedModal,
           "back dismisses the modal rather than reporting a plain pop");
    expect(harness.controller.depth() == 1, "the modal entry is gone");
    expect(harness.controller.active()->route_id == "orders", "the route below it is active");

    // Only `orders` remains, so the next back is the root edge, not a pop: the modal dismissal is
    // what kept the host from seeing AtRoot while a dialog was open.
    expect(harness.controller.back().outcome == BackOutcome::AtRoot,
           "with the modal gone the route below is the root edge");
}

void a_normal_pop_precedes_the_modal_check() {
    // The priority is about the *topmost* entry: a plain page above a modal is popped as a page,
    // because the modal is not on top any more (section 10.1's order is by position, not by kind).
    Harness harness;
    (void)harness.controller.push("/compose");
    (void)harness.controller.push("/orders");
    expect(harness.controller.back().outcome == BackOutcome::Popped,
           "a page above a modal is a normal pop");
}

void pop_to_removes_everything_above_the_target() {
    Harness harness;
    (void)harness.controller.push("/orders");
    const std::uint64_t orders_id = harness.controller.active()->entry_id;
    (void)harness.controller.push("/orders/42");
    (void)harness.controller.push("/settings");
    expect(harness.controller.depth() == 3, "three entries");

    // Section 2: "pop_to removes entries above the target."
    expect(harness.controller.pop_to(orders_id).has_value(), "pop_to the first entry succeeds");
    expect(harness.controller.depth() == 1, "two entries were removed");
    expect(harness.controller.active()->route_id == "orders", "the target is active");

    // Popping to the active entry is a successful no-op, not an error.
    expect(harness.controller.pop_to(orders_id).has_value(), "pop_to the active entry is a no-op");
    expect(harness.controller.depth() == 1, "and changes nothing");

    // An entry that is not in the stack is a named failure that changes nothing.
    const auto unknown = harness.controller.pop_to(9999);
    expect(!unknown.has_value(), "pop_to an unknown entry fails");
    expect(unknown.error().code == NavigationErrorCode::UnknownEntry, "with UnknownEntry");
    expect(harness.controller.depth() == 1, "and the stack is unchanged");
}

void reset_clears_history_but_keeps_a_root() {
    Harness harness;
    (void)harness.controller.push("/orders");
    (void)harness.controller.push("/orders/42");
    expect(harness.controller.depth() == 2, "two entries before reset");

    // Section 2: "reset clears it" - to the route the caller names, which is the only shape that
    // leaves a screen mounted.
    expect(harness.controller.reset_to("/").has_value(), "reset_to the root succeeds");
    expect(harness.controller.depth() == 1, "history is one entry");
    expect(harness.controller.active()->route_id == "home", "and it is the root");

    // `reset_to` validates its target like any other navigation.
    const auto bad = harness.controller.reset_to("/no/such/route");
    expect(!bad.has_value(), "an unknown reset target fails");
    expect(bad.error().code == NavigationErrorCode::NotFound, "with NotFound");
    expect(harness.controller.depth() == 1, "and the stack is unchanged");
    expect(harness.controller.active()->route_id == "home", "still at the root");
}

void replace_all_installs_a_whole_stack() {
    Harness harness;
    NavEntry home;
    home.route_id = "home";
    NavEntry detail;
    detail.route_id = "orders.detail";
    detail.params = {{"id", "42"}};
    const std::vector<NavEntry> entries{home, detail};
    expect(harness.controller.replace_all(entries, 1).has_value(), "replace_all succeeds");
    expect(harness.controller.depth() == 2, "both entries are installed");
    expect(harness.controller.active()->route_id == "orders.detail", "the named index is active");
    expect(harness.controller.active()->params.size() == 1, "with its params");

    // The active entry is last, so back pops it and lands on the entry below.
    expect(harness.controller.back().outcome == BackOutcome::Popped, "back pops the active entry");
    expect(harness.controller.active()->route_id == "home", "and lands on the entry below");

    // An empty list is refused (section 2's reset means "to the root", not "to nothing"), and an
    // index past the end is refused rather than clamped into a screen the caller did not ask for.
    expect(harness.controller.replace_all({}, 0).error().code == NavigationErrorCode::EmptyStack,
           "an empty replace_all is EmptyStack");
    const std::vector<NavEntry> one{home};
    expect(harness.controller.replace_all(one, 5).error().code == NavigationErrorCode::UnknownEntry,
           "an out-of-range active index is UnknownEntry");
    // An entry naming a route the table does not have is refused, so an invalid stack cannot be
    // installed.
    NavEntry bogus;
    bogus.route_id = "no.such.route";
    const std::vector<NavEntry> invalid{bogus};
    expect(harness.controller.replace_all(invalid, 0).error().code ==
               NavigationErrorCode::UnknownRouteId,
           "an unknown route id is UnknownRouteId");
}

void back_publishes_one_notification_per_tick() {
    Harness harness;
    RouteRecorder recorder;
    auto subscription = harness.controller.route().subscribe(&RouteRecorder::on_route, &recorder);

    (void)harness.controller.push("/orders");
    (void)harness.controller.push("/orders/42");
    expect(harness.controller.commit(), "the pushes publish");
    harness.scheduler.flush();
    expect(recorder.calls == 1, "one notification for the tick");
    expect(recorder.last_depth == 2, "with the depth after both pushes");

    // A back inside a tick is frame-boundary like any other change: the subscriber hears about it
    // once.
    expect(harness.controller.back().outcome == BackOutcome::Popped, "back pops");
    expect(recorder.calls == 1, "a back does not notify before the commit");
    expect(harness.controller.commit(), "the back publishes");
    harness.scheduler.flush();
    expect(recorder.calls == 2, "and delivers one notification");
    expect(recorder.last_depth == 1, "with the depth after the pop");
    expect(recorder.last_route_id == "orders", "and the route that is now active");
}

void a_push_and_pop_in_one_tick_publishes_nothing() {
    // Section 2's "no double commit" and AF4's coalescing together: a tick that ends where it
    // started wakes no subscriber, because the published value is unchanged.
    Harness harness;
    (void)harness.controller.push("/orders");
    expect(harness.controller.commit(), "the first push publishes");
    // Drain the flush before subscribing, so the only notification the recorder can see is one the
    // tick under test caused.
    harness.scheduler.flush();
    RouteRecorder recorder;
    auto subscription = harness.controller.route().subscribe(&RouteRecorder::on_route, &recorder);

    (void)harness.controller.push("/settings");
    expect(harness.controller.back().outcome == BackOutcome::Popped, "the pop succeeds");
    expect(!harness.controller.commit(), "a tick that ended on the same route publishes nothing");
    harness.scheduler.flush();
    expect(recorder.calls == 0, "and wakes nobody");
    expect(harness.controller.active()->route_id == "orders",
           "the stack still ends where it started");
}

} // namespace

int run_back_tests() {
    back_pops_the_top_entry();
    back_with_an_empty_stack_is_defined_behaviour();
    back_at_root_reports_the_edge_and_does_not_quit();
    a_modal_or_sheet_is_dismissed_before_the_route_below_it();
    a_normal_pop_precedes_the_modal_check();
    pop_to_removes_everything_above_the_target();
    reset_clears_history_but_keeps_a_root();
    replace_all_installs_a_whole_stack();
    back_publishes_one_notification_per_tick();
    a_push_and_pop_in_one_tick_publishes_nothing();
    return g_failures;
}

} // namespace nav_test
