// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/edit_buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tessera::ui::text {

// A committed edit state: the full UTF-8 text plus the exact caret/anchor bytes (the selection is
// the normalized pair, so a snapshot restores selection and caret together).
struct EditSnapshot {
    std::string text;
    EditOffset caret = 0;
    EditOffset anchor = 0;

    bool operator==(const EditSnapshot&) const = default;
};

// What produced a committed state. Only plain typing and plain (single-grapheme) Delete/Backspace
// runs coalesce within the window: consecutive commits of the same mergeable kind within
// `coalesce_seconds` collapse into one undo step. Everything else starts a new step: a newline
// (Enter), word-delete/word-backspace, paste/IME commits (EditText), composition commits,
// selection-replacing edits, and programmatic `set_text` (which also clears the history).
enum class EditCommitKind : std::uint8_t {
    Typing,
    Delete,
    WordDelete,
    Newline,
    Paste,
    Composition,
    Clipboard, // cut/paste: one step, never merged with typing or other edits
    Other,
};

struct EditHistoryConfig {
    // Bounded stacks: the oldest entries are evicted when either bound is exceeded. The byte budget
    // covers both stacks together and at least the newest entry is always kept.
    std::size_t max_entries = 128;
    std::size_t max_bytes = 256 * 1024;
    // Consecutive mergeable commits merge when they land within this deterministic window (driven
    // by `tick(now_seconds)`, never a wall clock). A caret move or selection change between commits
    // ends the run through `break_coalescing()`.
    double coalesce_seconds = 0.5;
};

// Bounded undo/redo history for one editable field (docs/specs/text-editing-v1.md section 12).
// The controller owns one history and calls `record_commit(before, kind)` after a committed edit;
// the API never records on undo/redo, so undoing does not grow the history. A new edit after undo
// truncates the redo tail. The history stores snapshots only, never the buffer, so it stays free of
// editing rules.
class EditHistory {
public:
    // Bounds must be > 0 and the coalescing window finite and >= 0; throws std::invalid_argument.
    explicit EditHistory(EditHistoryConfig config = {});

    // Records the state *before* a committed edit (the caller captured it before mutating the
    // buffer). Coalesces with the previous step according to the kind/time rule above.
    void record_commit(const EditSnapshot& before, EditCommitKind kind);

    [[nodiscard]] bool can_undo() const {
        return !undo_.empty();
    }
    [[nodiscard]] bool can_redo() const {
        return !redo_.empty();
    }

    // Returns the snapshot to restore (the caller applies it and supplies its current state so redo
    // can travel back), or nullopt when the corresponding stack is empty.
    std::optional<EditSnapshot> undo(const EditSnapshot& current);
    std::optional<EditSnapshot> redo(const EditSnapshot& current);

    // Non-edit activity (caret move, selection change): the next commit starts a new undo step.
    void break_coalescing();

    void clear();

    // Advances the deterministic coalescing clock. Time must be monotonic; a backwards timestamp is
    // clamped so a paused window cannot be replayed.
    void tick(double now_seconds);

    [[nodiscard]] std::size_t undo_entries() const {
        return undo_.size();
    }
    [[nodiscard]] std::size_t redo_entries() const {
        return redo_.size();
    }
    // Bytes retained by both stacks together (bounded by config().max_bytes).
    [[nodiscard]] std::size_t bytes() const {
        return undo_bytes_ + redo_bytes_;
    }
    [[nodiscard]] double now() const {
        return now_;
    }
    [[nodiscard]] const EditHistoryConfig& config() const {
        return config_;
    }

private:
    struct Entry {
        EditSnapshot snapshot;
        std::size_t bytes = 0;
    };

    void push(std::vector<Entry>& stack, std::size_t& bytes, const EditSnapshot& snapshot);
    void trim();

    EditHistoryConfig config_;
    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
    std::size_t undo_bytes_ = 0;
    std::size_t redo_bytes_ = 0;
    double now_ = 0.0;
    double last_commit_seconds_ = 0.0;
    EditCommitKind last_kind_ = EditCommitKind::Other;
    bool has_last_commit_ = false;
};

} // namespace tessera::ui::text
