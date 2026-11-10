// SPDX-License-Identifier: MIT
//
// UIA event model: bounded coalescing/dedup queue and the snapshot diff that derives
// StructureChanged, PropertyChanged and focus events. Platform independent: no COM, no client.

#include "a11y_uia_test_support.hpp"
#include "tessera/ui/accessibility/uia_events.hpp"
#include "tessera/ui/accessibility/uia_mapping.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace a11y_uia_test;
using tessera::ui::a11y::A11yIndex;
using tessera::ui::a11y::A11yNode;
using tessera::ui::a11y::A11ySnapshot;
using tessera::ui::a11y::kMaxQueuedUiaEvents;
using tessera::ui::a11y::kUiaBoundingRectanglePropertyId;
using tessera::ui::a11y::kUiaChildrenInvalidated;
using tessera::ui::a11y::kUiaChildrenReordered;
using tessera::ui::a11y::kUiaHelpTextPropertyId;
using tessera::ui::a11y::kUiaIsEnabledPropertyId;
using tessera::ui::a11y::kUiaNamePropertyId;
using tessera::ui::a11y::kUiaSelectionItemIsSelectedPropertyId;
using tessera::ui::a11y::kUiaValueValuePropertyId;
using tessera::ui::a11y::make_uia_focus_event;
using tessera::ui::a11y::make_uia_property_event;
using tessera::ui::a11y::make_uia_structure_event;
using tessera::ui::a11y::queue_snapshot_diff;
using tessera::ui::a11y::Role;
using tessera::ui::a11y::State;
using tessera::ui::a11y::uia_event_value;
using tessera::ui::a11y::UiaEvent;
using tessera::ui::a11y::UiaEventKind;
using tessera::ui::a11y::UiaEventQueue;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "a11y uia events: " << message << "\n";
    }
    return condition;
}

bool expect_eq(std::int64_t actual, std::int64_t expected, const char* message) {
    if (actual == expected) {
        return true;
    }
    std::cerr << "a11y uia events: " << message << " (actual=" << actual
              << ", expected=" << expected << ")\n";
    return false;
}

bool expect_text(std::string_view actual, std::string_view expected, const char* message) {
    if (actual == expected) {
        return true;
    }
    std::cerr << "a11y uia events: " << message << " (actual=\"" << actual << "\", expected=\""
              << expected << "\")\n";
    return false;
}

std::vector<UiaEvent> take_all(UiaEventQueue& queue) {
    std::vector<UiaEvent> events(kMaxQueuedUiaEvents);
    const std::size_t taken = queue.take(events);
    events.resize(taken);
    return events;
}

A11yNode focused_node(Role role, std::string_view name) {
    A11yNode node = make_node(role, name);
    node.states = State::Focusable | State::Focused;
    return node;
}

// The diff keys nodes by NodeId, so tests assign one id per pre-order position (id 0 means "no
// node"). Two snapshots built for the same layout therefore describe the same nodes.
A11ySnapshot with_ids(std::vector<A11yNode> nodes) {
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        nodes[index].id = index + 1;
    }
    return snapshot_of(std::move(nodes));
}

// Root + two buttons; `focused` selects which button carries the focus bit (0 = none).
A11ySnapshot focus_tree(A11yIndex focused) {
    A11yNode root = make_node(Role::Generic, "Root");
    A11yNode first =
        focused == 1 ? focused_node(Role::Button, "First") : make_node(Role::Button, "First");
    A11yNode second =
        focused == 2 ? focused_node(Role::Button, "Second") : make_node(Role::Button, "Second");
    link_children(root, {1, 2});
    return with_ids({root, first, second});
}

bool coalescing_and_dedup() {
    UiaEventQueue queue;
    if (!expect(queue.push(make_uia_property_event(3, kUiaNamePropertyId, "a")), "first push")) {
        return false;
    }
    if (!expect(queue.push(make_uia_property_event(3, kUiaNamePropertyId, "b")), "second push") ||
        !expect_eq(static_cast<std::int64_t>(queue.size()), 1, "same node+property coalesces") ||
        !expect_eq(static_cast<std::int64_t>(queue.coalesced()), 1, "one coalesce")) {
        return false;
    }
    const std::vector<UiaEvent> events = take_all(queue);
    if (!expect_eq(static_cast<std::int64_t>(events.size()), 1, "one event") ||
        !expect(events[0].kind == UiaEventKind::PropertyChanged, "property kind") ||
        !expect_eq(events[0].property, kUiaNamePropertyId, "property id") ||
        !expect_text(uia_event_value(events[0]), "b", "newest value wins")) {
        return false;
    }

    // The same value again is a dedup, not a new event.
    if (!expect(queue.push(make_uia_property_event(3, kUiaNamePropertyId, "b")), "third push") ||
        !expect(queue.push(make_uia_property_event(3, kUiaNamePropertyId, "b")), "fourth push")) {
        return false;
    }
    return expect_eq(static_cast<std::int64_t>(queue.size()), 1, "identical values dedup") &&
           expect_eq(static_cast<std::int64_t>(queue.deduped()), 1, "one dedup") &&
           expect_eq(static_cast<std::int64_t>(queue.coalesced()), 1, "no extra coalesce");
}

bool structure_and_focus_coalescing() {
    UiaEventQueue structure;
    (void)structure.push(make_uia_structure_event(2, kUiaChildrenInvalidated));
    (void)structure.push(make_uia_structure_event(2, kUiaChildrenReordered));
    const std::vector<UiaEvent> structure_events = take_all(structure);
    if (!expect_eq(static_cast<std::int64_t>(structure_events.size()), 1,
                   "one structure event per node") ||
        !expect_eq(structure_events[0].property, kUiaChildrenReordered,
                   "newest change type wins")) {
        return false;
    }

    UiaEventQueue focus;
    (void)focus.push(make_uia_focus_event(1));
    (void)focus.push(make_uia_focus_event(4));
    const std::vector<UiaEvent> focus_events = take_all(focus);
    return expect_eq(static_cast<std::int64_t>(focus_events.size()), 1,
                     "one pending focus event") &&
           expect_eq(static_cast<std::int64_t>(focus_events[0].node), 4, "newest focus wins") &&
           expect_eq(static_cast<std::int64_t>(focus.coalesced()), 1, "focus coalesced");
}

bool overflow_drops_the_oldest() {
    UiaEventQueue queue;
    for (std::size_t index = 0; index < kMaxQueuedUiaEvents; ++index) {
        const bool kept =
            queue.push(make_uia_property_event(static_cast<A11yIndex>(index), kUiaNamePropertyId));
        if (!expect(kept, "the queue accepts its capacity")) {
            return false;
        }
    }
    const bool kept_overflow = queue.push(
        make_uia_property_event(static_cast<A11yIndex>(kMaxQueuedUiaEvents), kUiaNamePropertyId));
    if (!expect(!kept_overflow, "the overflow reports the drop") ||
        !expect_eq(static_cast<std::int64_t>(queue.dropped()), 1, "one dropped") ||
        !expect_eq(static_cast<std::int64_t>(queue.size()),
                   static_cast<std::int64_t>(kMaxQueuedUiaEvents), "capacity is stable")) {
        return false;
    }
    const std::vector<UiaEvent> events = take_all(queue);
    return expect_eq(static_cast<std::int64_t>(events.size()),
                     static_cast<std::int64_t>(kMaxQueuedUiaEvents), "all slots served") &&
           expect_eq(static_cast<std::int64_t>(events.front().node), 1, "the oldest was dropped") &&
           expect_eq(static_cast<std::int64_t>(events.back().node),
                     static_cast<std::int64_t>(kMaxQueuedUiaEvents), "the newest is kept");
}

bool diff_of_identical_trees_is_empty() {
    const A11ySnapshot tree = focus_tree(1);
    UiaEventQueue queue;
    const auto diff = queue_snapshot_diff(tree, tree, queue);
    return expect_eq(static_cast<std::int64_t>(diff.queued), 0, "nothing queued") &&
           expect_eq(static_cast<std::int64_t>(queue.size()), 0, "queue empty") &&
           expect(!diff.focus_changed, "no focus change");
}

bool diff_reports_text_properties() {
    A11yNode root = make_node(Role::Generic, "Root");
    A11yNode field = make_node(Role::TextField, "Email", "a");
    link_children(root, {1});
    const A11ySnapshot previous = with_ids({root, field});
    a11y_uia_test::set_text(field.name, "E-mail");
    a11y_uia_test::set_text(field.value, "ab");
    const A11ySnapshot next = with_ids({root, field});

    UiaEventQueue queue;
    const auto diff = queue_snapshot_diff(previous, next, queue);
    const std::vector<UiaEvent> events = take_all(queue);
    return expect_eq(static_cast<std::int64_t>(diff.properties), 2, "two property events") &&
           expect_eq(static_cast<std::int64_t>(events.size()), 2, "two queued") &&
           expect(events[0].kind == UiaEventKind::PropertyChanged, "property kind") &&
           expect_eq(events[0].property, kUiaNamePropertyId, "name first") &&
           expect_text(uia_event_value(events[0]), "E-mail", "name value") &&
           expect_eq(events[1].property, kUiaValueValuePropertyId, "value second") &&
           expect_text(uia_event_value(events[1]), "ab", "value text");
}

bool diff_reports_state_and_bounds() {
    A11yNode root = make_node(Role::Generic, "Root");
    A11yNode check = make_node(Role::Checkbox, "Agree");
    check.states = State::Focusable;
    link_children(root, {1});
    const A11ySnapshot previous = with_ids({root, check});

    A11yNode changed = make_node(Role::Checkbox, "Agree");
    changed.states = State::Focusable | State::Disabled | State::Selected;
    changed.bounds = tessera::rendering::rhi::Rect{10.0f, 20.0f, 30.0f, 40.0f};
    const A11ySnapshot next = with_ids({root, changed});

    UiaEventQueue queue;
    const auto diff = queue_snapshot_diff(previous, next, queue);
    const std::vector<UiaEvent> events = take_all(queue);
    return expect_eq(static_cast<std::int64_t>(diff.properties), 3, "enabled, selected, bounds") &&
           expect_eq(events[0].property, kUiaIsEnabledPropertyId, "enabled first") &&
           expect_text(uia_event_value(events[0]), "0", "disabled value") &&
           expect_eq(events[1].property, kUiaSelectionItemIsSelectedPropertyId,
                     "selected second") &&
           expect_text(uia_event_value(events[1]), "1", "selected value") &&
           expect_eq(events[2].property, kUiaBoundingRectanglePropertyId, "bounds third");
}

bool diff_reports_structure_changes() {
    A11yNode root = make_node(Role::Generic, "Root");
    A11yNode first = make_node(Role::Button, "First");
    A11yNode second = make_node(Role::Button, "Second");
    link_children(root, {1, 2});
    const A11ySnapshot ordered = with_ids({root, first, second});

    // Reorder: same node set, different child order -> one Reordered event on the parent.
    A11yNode reordered_root = make_node(Role::Generic, "Root");
    link_children(reordered_root, {2, 1});
    const A11ySnapshot reordered = with_ids({reordered_root, first, second});
    UiaEventQueue reorder_queue;
    const auto reorder_diff = queue_snapshot_diff(ordered, reordered, reorder_queue);
    const std::vector<UiaEvent> reorder_events = take_all(reorder_queue);
    if (!expect_eq(static_cast<std::int64_t>(reorder_diff.structure), 1, "one structure event") ||
        !expect_eq(static_cast<std::int64_t>(reorder_events.size()), 1, "one queued") ||
        !expect(reorder_events[0].kind == UiaEventKind::StructureChanged, "structure kind") ||
        !expect_eq(reorder_events[0].property, kUiaChildrenReordered, "reordered change type") ||
        !expect_eq(static_cast<std::int64_t>(reorder_events[0].node), 0, "parent node")) {
        return false;
    }

    // Addition: the root invalidation and the parent child-list change coalesce to one event.
    A11yNode wide_root = make_node(Role::Generic, "Root");
    link_children(wide_root, {1, 2, 3});
    const A11yNode third = make_node(Role::Button, "Third");
    const A11ySnapshot wide = with_ids({wide_root, first, second, third});
    UiaEventQueue add_queue;
    const auto add_diff = queue_snapshot_diff(ordered, wide, add_queue);
    const std::vector<UiaEvent> add_events = take_all(add_queue);
    if (!expect(add_diff.nodes_added, "addition reported") ||
        !expect(add_diff.nodes_removed == false, "no removal") ||
        !expect_eq(static_cast<std::int64_t>(add_events.size()), 1,
                   "root invalidation coalesced with the child-list change") ||
        !expect_eq(add_events[0].property, kUiaChildrenInvalidated, "invalidated change type")) {
        return false;
    }

    // Removal: same shape, one event on the root.
    A11yNode narrow_root = make_node(Role::Generic, "Root");
    link_children(narrow_root, {1});
    const A11ySnapshot narrow = with_ids({narrow_root, first});
    UiaEventQueue remove_queue;
    const auto remove_diff = queue_snapshot_diff(ordered, narrow, remove_queue);
    const std::vector<UiaEvent> remove_events = take_all(remove_queue);
    return expect(remove_diff.nodes_removed, "removal reported") &&
           expect_eq(static_cast<std::int64_t>(remove_events.size()), 1, "one removal event") &&
           expect_eq(static_cast<std::int64_t>(remove_events[0].node), 0, "root owns the change");
}

bool diff_reports_focus_move() {
    UiaEventQueue queue;
    const auto diff = queue_snapshot_diff(focus_tree(1), focus_tree(2), queue);
    const std::vector<UiaEvent> events = take_all(queue);
    return expect(diff.focus_changed, "focus change reported") &&
           expect_eq(static_cast<std::int64_t>(events.size()), 1, "one focus event") &&
           expect(events[0].kind == UiaEventKind::FocusChanged, "focus kind") &&
           expect_eq(static_cast<std::int64_t>(events[0].node), 2, "new focus node");
}

bool diff_is_deterministic() {
    A11yNode root = make_node(Role::Generic, "Root");
    A11yNode field = make_node(Role::TextField, "Email", "a");
    link_children(root, {1});
    const A11ySnapshot previous = with_ids({root, field});
    a11y_uia_test::set_text(field.name, "E-mail");
    field.states = State::Disabled;
    field.bounds = tessera::rendering::rhi::Rect{1.0f, 2.0f, 3.0f, 4.0f};
    const A11ySnapshot next = with_ids({root, field});

    UiaEventQueue first;
    UiaEventQueue second;
    (void)queue_snapshot_diff(previous, next, first);
    (void)queue_snapshot_diff(previous, next, second);
    const std::vector<UiaEvent> lhs = take_all(first);
    const std::vector<UiaEvent> rhs = take_all(second);
    if (!expect_eq(static_cast<std::int64_t>(lhs.size()), static_cast<std::int64_t>(rhs.size()),
                   "same event count")) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (!expect(lhs[index].kind == rhs[index].kind, "same kind") ||
            !expect_eq(lhs[index].node, rhs[index].node, "same node") ||
            !expect_eq(lhs[index].property, rhs[index].property, "same property") ||
            !expect_text(uia_event_value(lhs[index]), uia_event_value(rhs[index]), "same value")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    if (!coalescing_and_dedup()) {
        return 1;
    }
    if (!structure_and_focus_coalescing()) {
        return 2;
    }
    if (!overflow_drops_the_oldest()) {
        return 3;
    }
    if (!diff_of_identical_trees_is_empty()) {
        return 4;
    }
    if (!diff_reports_text_properties()) {
        return 5;
    }
    if (!diff_reports_state_and_bounds()) {
        return 6;
    }
    if (!diff_reports_structure_changes()) {
        return 7;
    }
    if (!diff_reports_focus_move()) {
        return 8;
    }
    if (!diff_is_deterministic()) {
        return 9;
    }
    std::cout << "a11y_uia_events: ok\n";
    return 0;
}
