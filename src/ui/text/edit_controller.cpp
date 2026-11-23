// SPDX-License-Identifier: MIT

#include "edit_grapheme.hpp"
#include "tessera/ui/key_code.hpp"
#include "tessera/ui/text/edit_controller.hpp"

#include <algorithm>
#include <string_view>

namespace tessera::ui::text {
namespace {

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80U) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp < 0x10000U) {
        out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp <= 0x10FFFFU) {
        out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
}

std::string_view event_text(const Event& event) {
    // A `TextChange` carries its full payload through `text_view`; the IME/EditText family carries
    // the borrowed platform bytes. Both are borrowed for the synchronous dispatch only.
    if (event.text_view != nullptr) {
        return std::string_view(event.text_view, event.text_view_size);
    }
    return event.text_bytes();
}

// A single-line field never holds a line break: pasted or IME-committed newlines are dropped.
std::string without_line_breaks(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c != '\n' && c != '\r') {
            out.push_back(c);
        }
    }
    return out;
}

} // namespace

EditController::EditController(std::size_t max_bytes, EditHistoryConfig history_config)
    : buffer_(max_bytes),
      history_(history_config) {}

EditResult EditController::set_text(std::string_view text) {
    composition_.cancel();
    const EditResult result = buffer_.set_text(text);
    // Controlled value sync is a clean history boundary: the app replaced the field's value, so
    // undoing back across it would resurrect state the app no longer owns. No entry is recorded.
    history_.clear();
    last_result_ = result;
    changed_ = changed_ || result.changed;
    return result;
}

bool EditController::commit_text(std::string_view text) {
    std::string flattened;
    if (!multiline_) {
        flattened = without_line_breaks(text);
        text = flattened;
    }
    const EditSnapshot before = snapshot();
    if (composition_.active()) {
        const EditOffset start = composition_.start();
        const EditOffset end = composition_.end();
        composition_.cancel();
        return record_mutation(before, buffer_.replace_range(start, end, text),
                               EditCommitKind::Composition);
    }
    // A committed EditText is a paste or an IME commit; both always start a new undo step.
    return record_mutation(before, buffer_.insert(text), EditCommitKind::Paste);
}

EditSnapshot EditController::snapshot() const {
    return EditSnapshot{std::string(buffer_.text()), buffer_.caret(), buffer_.anchor()};
}

bool EditController::record_mutation(const EditSnapshot& before, EditResult result,
                                     EditCommitKind kind) {
    if (result.changed) {
        history_.record_commit(before, kind);
    }
    return accept(result, true);
}

void EditController::restore(const EditSnapshot& snapshot) {
    composition_.cancel();
    buffer_.set_text(snapshot.text);
    buffer_.set_selection(snapshot.anchor, snapshot.caret);
    // Restoring is a committed edit for the TextChange runtime (the controlled value refreshes) but
    // it is not recorded in the history: undo/redo move between existing snapshots only.
    EditResult restored;
    restored.status = EditStatus::Ok;
    restored.changed = true;
    restored.caret = buffer_.caret_position();
    restored.anchor = buffer_.anchor();
    accept(restored, true);
}

bool EditController::undo() {
    if (read_only_ || composition_.active()) {
        return false;
    }
    const std::optional<EditSnapshot> target = history_.undo(snapshot());
    if (!target.has_value()) {
        return false;
    }
    restore(*target);
    return true;
}

bool EditController::redo() {
    if (read_only_ || composition_.active()) {
        return false;
    }
    const std::optional<EditSnapshot> target = history_.redo(snapshot());
    if (!target.has_value()) {
        return false;
    }
    restore(*target);
    return true;
}

bool EditController::accept(EditResult result, bool text_edit) {
    last_result_ = result;
    changed_ = changed_ || result.changed;
    if (result.changed) {
        ++activity_count_;
        if (!text_edit) {
            // A caret move or selection change ends a typing/delete run, so the next edit starts a
            // new undo step instead of merging across the move.
            history_.break_coalescing();
        }
    }
    if (text_edit && result.changed) {
        ++committed_edits_;
    }
    return true;
}

EditResult EditController::place_caret_at(TextHit hit) {
    accept(buffer_.set_caret(hit.byte_offset), false);
    return last_result_;
}

EditResult EditController::extend_selection_to(TextHit hit) {
    accept(buffer_.set_selection(buffer_.anchor(), hit.byte_offset), false);
    return last_result_;
}

EditResult EditController::select_all() {
    accept(buffer_.select_all(), false);
    return last_result_;
}

bool EditController::handle_event(Event& event) {
    if (event.prevent_default || event.handled) {
        return false;
    }
    switch (event.type) {
    case EventType::Text: {
        if (read_only_ || composition_.active()) {
            return false;
        }
        const std::uint32_t cp = event.text_codepoint;
        if (cp == 0 || cp == '\r' || (cp < 0x20U && cp != '\n')) {
            return false;
        }
        if (cp == '\n' && !multiline_) {
            return false;
        }
        std::string bytes;
        append_utf8(bytes, cp);
        const EditCommitKind kind =
            cp == '\n' ? EditCommitKind::Newline
                       : (buffer_.has_selection() ? EditCommitKind::Other : EditCommitKind::Typing);
        const EditSnapshot before = snapshot();
        event.handled = true;
        return record_mutation(before, buffer_.insert(bytes), kind);
    }
    case EventType::EditText: {
        if (read_only_) {
            return false;
        }
        event.handled = true;
        return commit_text(event_text(event));
    }
    case EventType::CompositionStart: {
        if (read_only_ || composition_.active()) {
            return false;
        }
        composition_.begin(buffer_.selection_start(), buffer_.selection_end());
        event.handled = true;
        return true;
    }
    case EventType::CompositionUpdate: {
        if (read_only_) {
            return false;
        }
        const std::string_view text = event_text(event);
        if (!edit_detail::valid_utf8(text)) {
            return false;
        }
        if (!composition_.active()) {
            // An IME can commit one segment and start the next inside a single message (Windows
            // sends no second start): a non-empty update then begins a composition at the
            // current selection, exactly as CompositionStart would.
            if (text.empty()) {
                return false;
            }
            composition_.begin(buffer_.selection_start(), buffer_.selection_end());
        }
        composition_.update(text);
        event.handled = true;
        return true;
    }
    case EventType::CompositionEnd: {
        if (!composition_.active()) {
            return false;
        }
        // No committed `EditText` arrived: cancel restores the pre-composition state.
        composition_.cancel();
        event.handled = true;
        return true;
    }
    case EventType::KeyDown:
        return handle_key(event);
    default:
        return false;
    }
}

EditController::ClipboardResult EditController::copy(hal::Clipboard& clipboard) const {
    ClipboardResult result;
    if (!buffer_.has_selection()) {
        return result; // nothing selected: copy is a no-op and the chord stays unconsumed
    }
    const std::string selected(buffer_.selected_text());
    const auto wrote = clipboard.write_text(selected);
    if (!wrote.has_value()) {
        result.error = wrote.error();
        return result;
    }
    result.changed = true;
    result.bytes = selected.size();
    return result;
}

EditController::ClipboardResult EditController::cut(hal::Clipboard& clipboard) {
    ClipboardResult result;
    if (read_only_ || composition_.active() || !buffer_.has_selection()) {
        return result;
    }
    result = copy(clipboard);
    if (!result.changed) {
        return result;
    }
    const EditSnapshot before = snapshot();
    const EditOffset start = buffer_.selection_start();
    const EditOffset end = buffer_.selection_end();
    record_mutation(before, buffer_.replace_range(start, end, {}), EditCommitKind::Clipboard);
    result.changed = true;
    return result;
}

EditController::ClipboardResult EditController::paste(hal::Clipboard& clipboard) {
    ClipboardResult result;
    if (read_only_ || composition_.active()) {
        return result;
    }
    const auto read = clipboard.read_text();
    if (!read.has_value()) {
        result.error = read.error();
        return result;
    }
    const std::string text = multiline_ ? *read : without_line_breaks(*read);
    if (text.empty()) {
        return result; // empty clipboard: no edit, no history step, chord unconsumed
    }
    const EditSnapshot before = snapshot();
    result.bytes = text.size();
    const EditResult edit = buffer_.insert(text);
    if (edit.changed) {
        history_.record_commit(before, EditCommitKind::Clipboard);
    }
    result.changed = edit.changed;
    result.truncated = edit.truncated;
    accept(edit, true);
    return result;
}

namespace {

// The event's modifiers as the key map's set.
ModifierSet chord_modifiers(const Event& event) {
    ModifierSet set = ModifierSet::None;
    if (has_modifier(event.modifiers, Modifier::Control)) {
        set = set | ModifierSet::Control;
    }
    if (has_modifier(event.modifiers, Modifier::Shift)) {
        set = set | ModifierSet::Shift;
    }
    if (has_modifier(event.modifiers, Modifier::Alt)) {
        set = set | ModifierSet::Alt;
    }
    if (has_modifier(event.modifiers, Modifier::Meta)) {
        set = set | ModifierSet::Meta;
    }
    return set;
}

} // namespace

bool EditController::run_command(EditCommand command) {
    switch (command) {
    case EditCommand::None:
        return false;
    case EditCommand::DeleteBackward:
    case EditCommand::DeleteBackwardWord:
    case EditCommand::DeleteForward:
    case EditCommand::DeleteForwardWord: {
        if (read_only_) {
            return false;
        }
        const bool word =
            command == EditCommand::DeleteBackwardWord || command == EditCommand::DeleteForwardWord;
        const bool backward =
            command == EditCommand::DeleteBackward || command == EditCommand::DeleteBackwardWord;
        const EditCommitKind kind = word ? EditCommitKind::WordDelete : EditCommitKind::Delete;
        const EditSnapshot before = snapshot();
        const EditResult result =
            backward ? buffer_.delete_backward(word) : buffer_.delete_forward(word);
        return record_mutation(before, result, kind);
    }
    case EditCommand::InsertNewline: {
        if (read_only_ || !multiline_) {
            return false;
        }
        const EditSnapshot before = snapshot();
        const EditResult result = buffer_.insert("\n");
        return record_mutation(before, result, EditCommitKind::Newline);
    }
    case EditCommand::SelectAll:
        return accept(buffer_.select_all());
    case EditCommand::Copy:
    case EditCommand::Cut:
    case EditCommand::Paste: {
        // Consume only when the operation actually changed something (clipboard written or buffer
        // edited), so an empty selection/clipboard leaves the chord to the app.
        if (clipboard_ == nullptr) {
            return false;
        }
        ClipboardResult result;
        if (command == EditCommand::Copy) {
            result = copy(*clipboard_);
        } else if (command == EditCommand::Cut) {
            result = cut(*clipboard_);
        } else {
            result = paste(*clipboard_);
        }
        return result.changed;
    }
    case EditCommand::Undo:
        return undo();
    case EditCommand::Redo:
        return redo();
    // Everything else is a caret or selection move: accepted even when it lands on the same
    // position, which is what keeps the event consumed and the app from also acting on it.
    default:
        break;
    }
    const bool shift =
        command == EditCommand::MoveLeftExtend || command == EditCommand::MoveRightExtend ||
        command == EditCommand::MoveLeftWordExtend || command == EditCommand::MoveRightWordExtend ||
        command == EditCommand::MoveHomeExtend || command == EditCommand::MoveEndExtend ||
        command == EditCommand::MoveHomeDocumentExtend ||
        command == EditCommand::MoveEndDocumentExtend;
    const bool word =
        command == EditCommand::MoveLeftWord || command == EditCommand::MoveRightWord ||
        command == EditCommand::MoveLeftWordExtend || command == EditCommand::MoveRightWordExtend ||
        command == EditCommand::MoveHomeDocument || command == EditCommand::MoveEndDocument ||
        command == EditCommand::MoveHomeDocumentExtend ||
        command == EditCommand::MoveEndDocumentExtend;
    switch (command) {
    case EditCommand::MoveLeft:
    case EditCommand::MoveLeftWord:
    case EditCommand::MoveLeftExtend:
    case EditCommand::MoveLeftWordExtend:
        return accept(buffer_.move_left(word, shift));
    case EditCommand::MoveRight:
    case EditCommand::MoveRightWord:
    case EditCommand::MoveRightExtend:
    case EditCommand::MoveRightWordExtend:
        return accept(buffer_.move_right(word, shift));
    case EditCommand::MoveHome:
    case EditCommand::MoveHomeDocument:
    case EditCommand::MoveHomeExtend:
    case EditCommand::MoveHomeDocumentExtend:
        return accept(buffer_.move_home(word, shift));
    case EditCommand::MoveEnd:
    case EditCommand::MoveEndDocument:
    case EditCommand::MoveEndExtend:
    case EditCommand::MoveEndDocumentExtend:
        return accept(buffer_.move_end(word, shift));
    default:
        return false;
    }
}

bool EditController::handle_key(Event& event) {
    const EditCommand command = command_for(event.key, chord_modifiers(event));
    if (command == EditCommand::None) {
        return false;
    }
    if (!run_command(command)) {
        return false;
    }
    event.handled = true;
    return true;
}

} // namespace tessera::ui::text
