// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/event.hpp"
#include "tessera/ui/text/edit_buffer.hpp"
#include "tessera/ui/text/edit_history.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tessera::ui::text {

// Owns one editable field's text state and turns normalized scene input into editing commands
// (docs/specs/text-editing-v1.md section 12). The app/custom element owns the controller and
// forwards the focused node's events from its `on_event`:
//
//     if (controller.handle_event(event)) { /* changed / composing accessors updated */ }
//
// Handled events are marked `event.handled = true`. `prevent_default` is honoured first: an event
// that already carries `prevent_default` or `handled` is ignored, so a custom element suppresses
// the built-in editor by setting either flag before forwarding (or by not forwarding at all).
// Pointer/focus handling stays with the scene; this slice has no scene-graph changes.
// Key policy: Ctrl+Z undoes, Ctrl+Shift+Z and Ctrl+Y redo (the Windows/Linux convention; macOS
// uses Cmd+Z/Cmd+Shift+Z, which the platform layer maps onto the same normalized Control chord -
// see docs/specs/text-editing-v1.md section 12). A chord is consumed (handled) only when it
// actually changes the buffer, so an app can fall back to its own handling when the stack is empty;
// read-only fields ignore the chords. Alt/Meta combinations are not touched.
class EditController {
public:
    explicit EditController(std::size_t max_bytes = kMaxEditTextBytes,
                            EditHistoryConfig history_config = {});

    [[nodiscard]] EditBuffer& buffer() {
        return buffer_;
    }
    [[nodiscard]] const EditBuffer& buffer() const {
        return buffer_;
    }

    void set_read_only(bool read_only) {
        read_only_ = read_only;
    }
    [[nodiscard]] bool read_only() const {
        return read_only_;
    }
    // Multi-line fields accept Enter as a newline; single-line fields ignore it (submit is a form
    // concern and deferred).
    void set_multiline(bool multiline) {
        multiline_ = multiline;
    }
    [[nodiscard]] bool multiline() const {
        return multiline_;
    }

    // Composition (IME preedit) state. The preedit text is not part of the buffer until an
    // `EditText` commits it; `CompositionEnd` without a commit cancels and restores the state.
    [[nodiscard]] bool composing() const {
        return composing_;
    }
    [[nodiscard]] std::string_view preedit() const {
        return preedit_;
    }
    [[nodiscard]] EditOffset composition_start() const {
        return composition_start_;
    }
    [[nodiscard]] EditOffset composition_end() const {
        return composition_end_;
    }
    // The current IME composition for decoration emission (additive; task-72). `begin`/`end` are
    // the half-open buffer bytes the composition replaces (equal offsets = insertion at `begin`),
    // `text` is the uncommitted preedit bytes (borrowed until the next command) and `active` is
    // true between `CompositionStart` and the commit/cancel. Paint consumes this through
    // `TextDecoration::composition_active/preedit_*`; the commit path is unchanged (one
    // `TextChange` per committed edit).
    struct PreeditRange {
        EditOffset begin = 0;
        EditOffset end = 0;
        std::string_view text;
        bool active = false;
    };
    [[nodiscard]] PreeditRange preedit_range() const {
        if (!composing_) {
            return PreeditRange{buffer_.caret(), buffer_.caret(), {}, false};
        }
        return PreeditRange{composition_start_, composition_end_, preedit_, true};
    }

    // Set the controlled value from app state (normalizes/truncates like EditBuffer::set_text);
    // cancels an active composition first.
    EditResult set_text(std::string_view text);

    // Consumes `Text`, `EditText`, `CompositionStart/Update/End`, and the editing `KeyDown` keys
    // (Backspace/Delete, arrows, Home/End, Ctrl+A, Enter in multi-line fields). Returns true when
    // the controller consumed the event (and set `event.handled`).
    bool handle_event(Event& event);

    // Result of the most recent command (NoChange until the first command).
    [[nodiscard]] const EditResult& last_result() const {
        return last_result_;
    }
    // True once for every change since the last call; the onChange wiring uses this to coalesce.
    bool take_changed() {
        const bool changed = changed_;
        changed_ = false;
        return changed;
    }
    // Number of committed text mutations: insert/replace/delete/backspace/delete, multi-line Enter,
    // and composition commits. Selection-only commands, preedit updates and the external
    // controlled-value `set_text` sync do not increment it. The runtime change-event layer
    // (text_input_runtime.hpp) turns each increment into one TextChange scene event.
    [[nodiscard]] std::uint64_t committed_edit_count() const {
        return committed_edits_;
    }
    // Undo/redo (docs/specs/text-editing-v1.md section 12): bounded snapshot history owned by the
    // controller. Undo/redo restore the text and the caret/selection, count as committed edits for
    // the TextChange runtime (the controlled value refreshes) but never push history themselves. A
    // new edit after undo truncates the redo tail; `set_text` clears the history (controlled sync
    // is not undoable); read-only fields ignore both commands.
    bool undo();
    bool redo();
    [[nodiscard]] bool can_undo() const {
        return history_.can_undo();
    }
    [[nodiscard]] bool can_redo() const {
        return history_.can_redo();
    }
    void clear_history() {
        history_.clear();
    }
    // Advances the deterministic coalescing window (call once per fixed-step tick). No wall clock.
    void tick(double now_seconds) {
        history_.tick(now_seconds);
    }
    [[nodiscard]] const EditHistory& history() const {
        return history_;
    }
    // Number of commands that changed the text, the selection or the caret since construction. The
    // caret blink owner compares this with its last seen value and calls CaretBlink::reset() on
    // typing/caret moves/selection changes (docs/specs/text-editing-v1.md section 12).
    [[nodiscard]] std::uint64_t activity_count() const {
        return activity_count_;
    }
    // Blink policy helper: the caret blinks only in a focused, editable field with a collapsed
    // selection (platform-typical: a non-collapsed selection hides the caret). Feed the result to
    // CaretBlink::set_active(); read-only and unfocused fields keep the caret hidden.
    [[nodiscard]] bool caret_blink_active(bool focused) const {
        return focused && !read_only_ && !buffer_.has_selection();
    }

private:
    bool handle_key(Event& event);
    bool commit_text(std::string_view text);
    bool accept(EditResult result, bool text_edit = false);
    [[nodiscard]] EditSnapshot snapshot() const;
    bool record_mutation(const EditSnapshot& before, EditResult result, EditCommitKind kind);
    void restore(const EditSnapshot& snapshot);

    EditBuffer buffer_;
    EditHistory history_;
    std::string preedit_;
    EditOffset composition_start_ = 0;
    EditOffset composition_end_ = 0;
    bool composing_ = false;
    bool read_only_ = false;
    bool multiline_ = false;
    EditResult last_result_;
    bool changed_ = false;
    std::uint64_t committed_edits_ = 0;
    std::uint64_t activity_count_ = 0;
};

} // namespace tessera::ui::text
