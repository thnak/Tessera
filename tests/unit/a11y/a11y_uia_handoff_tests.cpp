// SPDX-License-Identifier: MIT
//
// Snapshot handoff: one slot, drop-oldest with a counter, newest-tree-wins, and buffer reuse
// between `take` and `publish`.

#include "a11y_uia_test_support.hpp"
#include "tessera/ui/accessibility/uia_handoff.hpp"

#include <array>
#include <iostream>
#include <string>

namespace {

using namespace a11y_uia_test;
using tessera::ui::a11y::A11ySnapshot;
using tessera::ui::a11y::kMaxNodes;
using tessera::ui::a11y::Role;
using tessera::ui::a11y::SnapshotHandoff;

A11ySnapshot named_snapshot(std::string_view name) {
    return snapshot_of({make_node(Role::Text, name)});
}

bool empty_handoff() {
    SnapshotHandoff handoff;
    A11ySnapshot out;
    return expect(!handoff.has_pending(), "a fresh handoff has nothing pending") &&
           expect(!handoff.take(out), "taking from an empty handoff reports nothing") &&
           expect_eq(static_cast<std::int64_t>(handoff.stats().published), 0, "no publish yet") &&
           expect_eq(static_cast<std::int64_t>(handoff.stats().taken), 0, "no take yet");
}

bool publish_take_round_trip() {
    SnapshotHandoff handoff;
    handoff.publish(named_snapshot("first"));
    A11ySnapshot out;
    const bool pending = handoff.has_pending();
    const bool taken = handoff.take(out);
    const auto stats = handoff.stats();
    return expect(pending, "a publish marks the slot pending") && expect(taken, "take moves it") &&
           expect(out.nodes.size() == 1, "one node arrives") &&
           expect_equal(tessera::ui::a11y::name_of(out.nodes[0]), "first", "node text") &&
           expect_eq(static_cast<std::int64_t>(stats.published), 1, "published counted") &&
           expect_eq(static_cast<std::int64_t>(stats.taken), 1, "taken counted") &&
           expect_eq(static_cast<std::int64_t>(stats.dropped), 0, "nothing dropped") &&
           expect(!handoff.has_pending(), "the slot is empty after take") &&
           expect(!handoff.take(out), "a second take with no publish reports nothing");
}

bool drop_oldest_keeps_newest() {
    SnapshotHandoff handoff;
    handoff.publish(named_snapshot("A"));
    handoff.publish(named_snapshot("B"));
    handoff.publish(named_snapshot("C"));
    A11ySnapshot out;
    const bool taken = handoff.take(out);
    const auto stats = handoff.stats();
    return expect(taken, "the newest snapshot is available") &&
           expect_equal(tessera::ui::a11y::name_of(out.nodes[0]), "C", "newest tree wins") &&
           expect_eq(static_cast<std::int64_t>(stats.published), 3, "all publishes counted") &&
           expect_eq(static_cast<std::int64_t>(stats.taken), 1, "one take") &&
           expect_eq(static_cast<std::int64_t>(stats.dropped), 2, "two replaced publishes dropped");
}

bool buffers_are_reused() {
    SnapshotHandoff handoff;
    handoff.publish(named_snapshot("A"));
    A11ySnapshot out;
    (void)handoff.take(out);
    const std::size_t first_capacity = out.nodes.capacity();
    handoff.publish(named_snapshot("B"));
    const bool taken = handoff.take(out);
    return expect(first_capacity >= 1, "the consumer buffer holds the first snapshot") &&
           expect(taken, "the second publish is taken") &&
           expect_equal(tessera::ui::a11y::name_of(out.nodes[0]), "B", "second tree arrives") &&
           expect(out.nodes.capacity() >= first_capacity,
                  "the consumer buffer is reused instead of reallocated");
}

bool clear_drops_the_slot() {
    SnapshotHandoff handoff;
    handoff.publish(named_snapshot("A"));
    handoff.clear();
    A11ySnapshot out;
    return expect(!handoff.has_pending(), "clear empties the slot") &&
           expect(!handoff.take(out), "clear leaves nothing to take") &&
           expect_eq(static_cast<std::int64_t>(handoff.stats().taken), 0, "clear is not a take");
}

} // namespace

int main() {
    if (!empty_handoff()) {
        return 1;
    }
    if (!publish_take_round_trip()) {
        return 2;
    }
    if (!drop_oldest_keeps_newest()) {
        return 3;
    }
    if (!buffers_are_reused()) {
        return 4;
    }
    if (!clear_drops_the_slot()) {
        return 5;
    }
    std::cout << "a11y_uia_handoff: ok\n";
    return 0;
}
