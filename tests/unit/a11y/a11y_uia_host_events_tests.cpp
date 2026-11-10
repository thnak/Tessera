// SPDX-License-Identifier: MIT
//
// Host-level UIA events: a dirty publish derives coalesced events, a flush job raises them on the
// bridge thread, and a static window raises nothing. The fake platform records the batches, so the
// test needs no COM and no client.

#include "a11y_uia_host_fixture.hpp"
#include "a11y_uia_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace a11y_uia_host_test;
using tessera::ui::a11y::A11yNode;
using tessera::ui::a11y::A11ySnapshot;
using tessera::ui::a11y::kUiaNamePropertyId;
using tessera::ui::a11y::Role;
using tessera::ui::a11y::uia_event_value;
using tessera::ui::a11y::UiaEvent;
using tessera::ui::a11y::UiaEventKind;
using tessera::ui::a11y::UiaHost;
using tessera::ui::a11y::UiaHostTickStats;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "a11y uia host events: " << message << "\n";
    }
    return condition;
}

bool expect_eq(std::int64_t actual, std::int64_t expected, const char* message) {
    if (actual == expected) {
        return true;
    }
    std::cerr << "a11y uia host events: " << message << " (actual=" << actual
              << ", expected=" << expected << ")\n";
    return false;
}

bool expect_text(std::string_view actual, std::string_view expected, const char* message) {
    if (actual == expected) {
        return true;
    }
    std::cerr << "a11y uia host events: " << message << " (actual=\"" << actual << "\", expected=\""
              << expected << "\")\n";
    return false;
}

// Root (id 1) + one button (id 2). The ids make the diff see the same node across publishes.
void build_button_tree(A11ySnapshot& out, std::string_view button_name) {
    A11yNode root = a11y_uia_test::make_node(Role::Generic, "Root");
    A11yNode button = a11y_uia_test::make_node(Role::Button, button_name);
    root.id = 1;
    button.id = 2;
    a11y_uia_test::link_children(root, {1});
    out.nodes.assign({root, button});
}

// Starts the host with the worker parked in its message pump, so a posted flush job cannot run
// until the test opens the gate. That is what makes "pending" assertions deterministic.
bool start_parked(HostHarness& harness) {
    harness.apartment->block_pump();
    return expect(harness.host->start().has_value(), "start succeeds") &&
           expect(harness.host->bridge().step().has_value(), "the start hook ran") &&
           expect(harness.host->ready(), "the root is published");
}

bool tick_raising(HostHarness& harness, std::string_view name, UiaHostTickStats& stats) {
    harness.host->mark_dirty();
    stats = harness.host->tick([name](A11ySnapshot& out) { build_button_tree(out, name); });
    return expect(stats.published, "the dirty tick published");
}

bool static_tick_raises_nothing() {
    auto harness = make_host();
    if (!expect(harness->host->start().has_value(), "start succeeds") ||
        !expect(harness->host->bridge().step().has_value(), "start hook ran")) {
        return false;
    }
    const UiaHostTickStats stats = harness->host->tick([](A11ySnapshot&) {});
    return expect(!stats.published, "nothing published") &&
           expect_eq(static_cast<std::int64_t>(stats.events_queued), 0, "no events queued") &&
           expect_eq(static_cast<std::int64_t>(stats.events_pending), 0, "no events pending") &&
           expect_eq(static_cast<std::int64_t>(harness->platform->batches().size()), 0,
                     "nothing raised");
}

bool flush_raises_on_the_bridge_thread() {
    auto harness = make_host();
    if (!start_parked(*harness)) {
        return false;
    }
    UiaHostTickStats stats;
    if (!tick_raising(*harness, "Save", stats)) {
        return false;
    }
    if (!expect_eq(static_cast<std::int64_t>(stats.events_queued), 1,
                   "the first publish invalidates the root once") ||
        !expect_eq(static_cast<std::int64_t>(stats.events_pending), 1,
                   "the event waits for the flush job") ||
        !expect_eq(static_cast<std::int64_t>(harness->platform->batches().size()), 0,
                   "nothing is raised from the tick itself")) {
        harness->apartment->open_pump();
        return false;
    }

    harness->apartment->open_pump();
    if (!expect(harness->host->bridge().step().has_value(), "barrier runs the posted flush")) {
        return false;
    }
    const auto batches = harness->platform->batches();
    const bool ok =
        expect_eq(static_cast<std::int64_t>(batches.size()), 1, "one raised batch") &&
        expect(batches[0].thread == harness->host->bridge().bridge_thread(),
               "the batch is raised on the bridge thread") &&
        expect_eq(static_cast<std::int64_t>(batches[0].events.size()), 1,
                  "one event in the batch") &&
        expect(batches[0].events[0].kind == UiaEventKind::StructureChanged,
               "the first publish is a structure change") &&
        expect_eq(static_cast<std::int64_t>(batches[0].events[0].node), 0, "root node") &&
        expect_eq(static_cast<std::int64_t>(harness->host->pending_events()), 0,
                  "the queue was drained") &&
        expect_eq(static_cast<std::int64_t>(harness->host->raised_batches()), 1,
                  "one successful raise") &&
        expect_eq(static_cast<std::int64_t>(harness->host->failed_raises()), 0, "no failures");
    harness->host->stop();
    return ok;
}

bool identical_tree_raises_no_events() {
    auto harness = make_host();
    if (!start_parked(*harness)) {
        return false;
    }
    UiaHostTickStats first;
    if (!tick_raising(*harness, "Save", first)) {
        harness->apartment->open_pump();
        return false;
    }
    harness->apartment->open_pump();
    if (!expect(harness->host->bridge().step().has_value(), "first flush")) {
        return false;
    }
    // Rebuild the same logical tree: ids match, so the diff has nothing to report.
    harness->host->mark_dirty();
    const UiaHostTickStats second =
        harness->host->tick([](A11ySnapshot& out) { build_button_tree(out, "Save"); });
    if (!expect(second.published, "the dirty tick published") ||
        !expect_eq(static_cast<std::int64_t>(second.events_queued), 0,
                   "an unchanged tree queues nothing") ||
        !expect_eq(static_cast<std::int64_t>(second.events_pending), 0, "nothing pending")) {
        return false;
    }
    if (!expect(harness->host->bridge().step().has_value(), "second barrier")) {
        return false;
    }
    harness->host->stop();
    return expect_eq(static_cast<std::int64_t>(harness->platform->batches().size()), 1,
                     "no second batch");
}

bool property_change_raises_a_property_event() {
    auto harness = make_host();
    if (!start_parked(*harness)) {
        return false;
    }
    UiaHostTickStats first;
    if (!tick_raising(*harness, "Save", first)) {
        harness->apartment->open_pump();
        return false;
    }
    harness->apartment->open_pump();
    if (!expect(harness->host->bridge().step().has_value(), "first flush")) {
        return false;
    }
    harness->apartment->block_pump();
    UiaHostTickStats second;
    if (!tick_raising(*harness, "Store", second)) {
        harness->apartment->open_pump();
        return false;
    }
    const bool queued =
        expect_eq(static_cast<std::int64_t>(second.events_queued), 1, "one property event") &&
        expect_eq(static_cast<std::int64_t>(second.events_pending), 1,
                  "the property event waits for a flush");
    harness->apartment->open_pump();
    if (!queued ||
        !expect(harness->host->bridge().step().has_value(), "flush the property event")) {
        return false;
    }
    const auto batches = harness->platform->batches();
    const bool ok =
        expect_eq(static_cast<std::int64_t>(batches.size()), 2, "a second batch") &&
        expect_eq(static_cast<std::int64_t>(batches[1].events.size()), 1, "one event") &&
        expect(batches[1].events[0].kind == UiaEventKind::PropertyChanged, "property kind") &&
        expect_eq(batches[1].events[0].property, kUiaNamePropertyId, "name property") &&
        expect_eq(static_cast<std::int64_t>(batches[1].events[0].node), 1, "the button node") &&
        expect_text(uia_event_value(batches[1].events[0]), "Store", "new name") &&
        expect(batches[1].thread == harness->host->bridge().bridge_thread(),
               "raised on the bridge thread");
    harness->host->stop();
    return ok;
}

bool focus_move_raises_a_focus_event() {
    auto harness = make_host();
    if (!start_parked(*harness)) {
        return false;
    }
    const auto build = [](std::size_t focused_index) {
        return [focused_index](A11ySnapshot& out) {
            A11yNode root = a11y_uia_test::make_node(Role::Generic, "Root");
            A11yNode first = a11y_uia_test::make_node(Role::Button, "First");
            A11yNode second = a11y_uia_test::make_node(Role::Button, "Second");
            root.id = 1;
            first.id = 2;
            second.id = 3;
            const auto focused =
                tessera::ui::a11y::State::Focusable | tessera::ui::a11y::State::Focused;
            if (focused_index == 1) {
                first.states = focused;
            } else if (focused_index == 2) {
                second.states = focused;
            }
            out.nodes.assign({root, first, second});
        };
    };

    harness->host->mark_dirty();
    const auto published = harness->host->tick(build(1));
    harness->apartment->open_pump();
    if (!expect(published.published, "the focus tree published") ||
        !expect(harness->host->bridge().step().has_value(), "first flush")) {
        return false;
    }
    harness->apartment->block_pump();
    harness->host->mark_dirty();
    const UiaHostTickStats second = harness->host->tick(build(2));
    harness->apartment->open_pump();
    if (!expect(second.published, "the second tree published") ||
        !expect_eq(static_cast<std::int64_t>(second.events_queued), 1, "one focus event") ||
        !expect(harness->host->bridge().step().has_value(), "flush the focus event")) {
        return false;
    }
    const auto batches = harness->platform->batches();
    const bool ok =
        expect_eq(static_cast<std::int64_t>(batches.size()), 2, "two batches") &&
        expect_eq(static_cast<std::int64_t>(batches[1].events.size()), 1,
                  "one focus event in the batch") &&
        expect(batches[1].events[0].kind == UiaEventKind::FocusChanged, "focus kind") &&
        expect_eq(static_cast<std::int64_t>(batches[1].events[0].node), 2, "new focus node");
    harness->host->stop();
    return ok;
}

} // namespace

int main() {
    if (!static_tick_raises_nothing()) {
        return 1;
    }
    if (!flush_raises_on_the_bridge_thread()) {
        return 2;
    }
    if (!identical_tree_raises_no_events()) {
        return 3;
    }
    if (!property_change_raises_a_property_event()) {
        return 4;
    }
    if (!focus_move_raises_a_focus_event()) {
        return 5;
    }
    std::cout << "a11y_uia_host_events: ok\n";
    return 0;
}
