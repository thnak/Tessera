// SPDX-License-Identifier: MIT
//
// EditHistory unit tests (task-69): bounded undo/redo snapshots, the deterministic coalescing
// window and kinds, entry/byte eviction, redo truncation, monotonic ticking and validation.

#include "tessera/ui/text/edit_history.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::ui::text::EditCommitKind;
using tessera::ui::text::EditHistory;
using tessera::ui::text::EditHistoryConfig;
using tessera::ui::text::EditSnapshot;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_edit_history: " << message << "\n";
    }
    return condition;
}

EditSnapshot snapshot_of(std::string text, std::size_t caret, std::size_t anchor) {
    return EditSnapshot{std::move(text), caret, anchor};
}

bool rejects(EditHistoryConfig config) {
    try {
        EditHistory history(config);
        (void)history;
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

bool history_basics() {
    EditHistory history;
    const EditSnapshot empty = snapshot_of("", 0, 0);
    const EditSnapshot one = snapshot_of("a", 1, 1);
    history.record_commit(empty, EditCommitKind::Typing);
    if (!expect(history.can_undo(), "one commit can be undone") ||
        !expect(!history.can_redo(), "a fresh history has no redo")) {
        return false;
    }
    const auto undone = history.undo(one);
    if (!expect(undone.has_value() && *undone == empty, "undo restores the recorded state") ||
        !expect(!history.can_undo(), "undo drained the stack") ||
        !expect(history.can_redo(), "undo filled the redo stack")) {
        return false;
    }
    const auto redone = history.redo(empty);
    return expect(redone.has_value() && *redone == one, "redo restores the undone state") &&
           expect(history.can_undo() && !history.can_redo(), "redo moves back to the undo stack") &&
           expect(history.undo(snapshot_of("b", 1, 1)).has_value(), "undo works after redo") &&
           expect(history.redo(snapshot_of("", 0, 0)).has_value(),
                  "redo works after a second undo") &&
           expect(history.can_undo(),
                  "the redo pushed the pre-redo state back onto the undo stack");
}

bool coalescing_window() {
    EditHistory history;
    history.record_commit(snapshot_of("", 0, 0), EditCommitKind::Typing);
    history.tick(0.3);
    history.record_commit(snapshot_of("a", 1, 1), EditCommitKind::Typing);
    if (!expect(history.undo_entries() == 1, "typing within the window merges")) {
        return false;
    }
    history.tick(0.3 + 0.6);
    history.record_commit(snapshot_of("ab", 2, 2), EditCommitKind::Typing);
    if (!expect(history.undo_entries() == 2, "typing past the window starts a new step")) {
        return false;
    }
    history.break_coalescing();
    history.record_commit(snapshot_of("abc", 3, 3), EditCommitKind::Typing);
    if (!expect(history.undo_entries() == 3, "break_coalescing ends the run")) {
        return false;
    }
    // Mergeable deletes coalesce with each other, but never with typing.
    EditHistory deletes;
    deletes.record_commit(snapshot_of("abc", 3, 3), EditCommitKind::Delete);
    deletes.tick(0.2);
    deletes.record_commit(snapshot_of("ab", 2, 2), EditCommitKind::Delete);
    if (!expect(deletes.undo_entries() == 1, "consecutive deletes merge")) {
        return false;
    }
    deletes.tick(0.2);
    deletes.record_commit(snapshot_of("a", 1, 1), EditCommitKind::Typing);
    return expect(deletes.undo_entries() == 2, "typing after a delete run starts a new step");
}

bool non_mergeable_kinds() {
    EditHistory history;
    history.record_commit(snapshot_of("a", 1, 1), EditCommitKind::Typing);
    history.record_commit(snapshot_of("ab", 2, 2), EditCommitKind::WordDelete);
    history.record_commit(snapshot_of("abc", 3, 3), EditCommitKind::Newline);
    history.record_commit(snapshot_of("abcd", 4, 4), EditCommitKind::Paste);
    history.record_commit(snapshot_of("abcde", 5, 5), EditCommitKind::Composition);
    history.record_commit(snapshot_of("abcdef", 6, 6), EditCommitKind::Other);
    return expect(history.undo_entries() == 6,
                  "word-delete/newline/paste/composition/other never merge");
}

bool cap_and_budget() {
    EditHistory small(EditHistoryConfig{2, 1 << 20, 10.0});
    for (int index = 0; index < 5; ++index) {
        small.record_commit(snapshot_of(std::string(1, 'a' + static_cast<char>(index)),
                                        static_cast<std::size_t>(index), index),
                            EditCommitKind::Other);
    }
    if (!expect(small.undo_entries() == 2, "entry cap evicts the oldest states")) {
        return false;
    }
    const auto first = small.undo(snapshot_of("zzz", 3, 3));
    const auto second = small.undo(snapshot_of("yyy", 3, 3));
    if (!expect(first.has_value() && first->text == "e", "newest two states survive") ||
        !expect(second.has_value() && second->text == "d", "second-newest state survives") ||
        !expect(!small.can_undo(), "evicted states are gone")) {
        return false;
    }
    // The byte budget keeps at least the newest entry even when it alone exceeds the budget.
    EditHistory tiny(EditHistoryConfig{100, 4, 10.0});
    tiny.record_commit(snapshot_of("123456", 6, 6), EditCommitKind::Other);
    tiny.record_commit(snapshot_of("abcdef", 6, 6), EditCommitKind::Other);
    if (!expect(tiny.undo_entries() >= 1, "a single oversized snapshot is kept") ||
        !expect(tiny.bytes() <= 4 || tiny.undo_entries() == 1,
                "the byte budget evicts down to the newest entry")) {
        return false;
    }
    return expect(rejects(EditHistoryConfig{0, 100, 0.5}), "zero entry cap rejected") &&
           expect(rejects(EditHistoryConfig{10, 0, 0.5}), "zero byte budget rejected") &&
           expect(rejects(EditHistoryConfig{10, 100, -1.0}), "negative window rejected") &&
           expect(rejects(EditHistoryConfig{10, 100, std::numeric_limits<double>::infinity()}),
                  "infinite window rejected");
}

bool undo_does_not_grow_history() {
    EditHistory history;
    history.record_commit(snapshot_of("", 0, 0), EditCommitKind::Typing);
    history.record_commit(snapshot_of("a", 1, 1), EditCommitKind::Other);
    const std::size_t before = history.undo_entries();
    history.undo(snapshot_of("ab", 2, 2));
    const std::size_t after_undo = history.undo_entries();
    history.redo(snapshot_of("", 0, 0));
    return expect(after_undo == before - 1, "undo pops without pushing") &&
           expect(history.undo_entries() == before, "redo returns the entry to the undo stack") &&
           expect(history.redo_entries() == 0, "redo consumed its entry");
}

bool redo_truncation() {
    EditHistory history;
    history.record_commit(snapshot_of("", 0, 0), EditCommitKind::Other);
    history.undo(snapshot_of("a", 1, 1));
    if (!expect(history.can_redo(), "undo created a redo entry")) {
        return false;
    }
    history.record_commit(snapshot_of("", 0, 0), EditCommitKind::Typing);
    return expect(!history.can_redo(), "a new edit truncates the redo tail") &&
           expect(history.undo_entries() == 1, "the new edit is recorded");
}

bool ticks_are_monotonic() {
    EditHistory history;
    history.tick(5.0);
    if (!expect(std::abs(history.now() - 5.0) < 1.0e-9, "tick advances the clock")) {
        return false;
    }
    history.tick(2.0);
    if (!expect(std::abs(history.now() - 5.0) < 1.0e-9, "a backwards tick is clamped")) {
        return false;
    }
    bool threw = false;
    try {
        history.tick(std::numeric_limits<double>::quiet_NaN());
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    return expect(threw, "a non-finite tick is rejected");
}

} // namespace

int main() {
    if (!history_basics()) {
        return 1;
    }
    if (!coalescing_window()) {
        return 2;
    }
    if (!non_mergeable_kinds()) {
        return 3;
    }
    if (!cap_and_budget()) {
        return 4;
    }
    if (!undo_does_not_grow_history()) {
        return 5;
    }
    if (!redo_truncation()) {
        return 6;
    }
    if (!ticks_are_monotonic()) {
        return 7;
    }

    std::cout << "text_edit_history: ok\n";
    return 0;
}
