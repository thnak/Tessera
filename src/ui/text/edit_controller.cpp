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
    // Clamp to the event's own storage: text_size from a directly constructed Event can exceed it.
    const std::size_t size = std::min<std::size_t>(event.text_size, event.text.size());
    return std::string_view(event.text.data(), size);
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
    composing_ = false;
    preedit_.clear();
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
    if (composing_) {
        const EditOffset start = composition_start_;
        const EditOffset end = composition_end_;
        composing_ = false;
        preedit_.clear();
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
    composing_ = false;
    preedit_.clear();
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
    if (read_only_ || composing_) {
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
    if (read_only_ || composing_) {
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

bool EditController::handle_event(Event& event) {
    if (event.prevent_default || event.handled) {
        return false;
    }
    switch (event.type) {
    case EventType::Text: {
        if (read_only_ || composing_) {
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
        if (read_only_ || composing_) {
            return false;
        }
        composing_ = true;
        preedit_.clear();
        composition_start_ = buffer_.selection_start();
        composition_end_ = buffer_.selection_end();
        event.handled = true;
        return true;
    }
    case EventType::CompositionUpdate: {
        if (read_only_ || !composing_) {
            return false;
        }
        const std::string_view text = event_text(event);
        if (!edit_detail::valid_utf8(text)) {
            return false;
        }
        preedit_.assign(text.data(), text.size());
        event.handled = true;
        return true;
    }
    case EventType::CompositionEnd: {
        if (!composing_) {
            return false;
        }
        // No committed `EditText` arrived: cancel restores the pre-composition state.
        composing_ = false;
        preedit_.clear();
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
    if (read_only_ || composing_ || !buffer_.has_selection()) {
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
    if (read_only_ || composing_) {
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

bool EditController::handle_key(Event& event) {
    const std::uint32_t key = event.key;
    const bool control = has_modifier(event.modifiers, Modifier::Control);
    const bool shift = has_modifier(event.modifiers, Modifier::Shift);
    const bool alt = has_modifier(event.modifiers, Modifier::Alt);
    const bool meta = has_modifier(event.modifiers, Modifier::Meta);

    if (control && !alt && !meta) {
        // Clipboard chords: consume only when the operation actually changed something (clipboard
        // written or buffer edited), so an empty selection/clipboard leaves the chord to the app.
        const auto run_chord = [&](hal::Clipboard& clipboard) {
            if (key == 'C') {
                return copy(clipboard).changed;
            }
            if (key == 'X') {
                return cut(clipboard).changed;
            }
            if (key == 'V') {
                return paste(clipboard).changed;
            }
            return false;
        };
        if ((key == 'C' || key == 'X' || key == 'V') && clipboard_ != nullptr) {
            if (run_chord(*clipboard_)) {
                event.handled = true;
                return true;
            }
            return false;
        }
        if (key == 'Z') {
            if (shift ? redo() : undo()) {
                event.handled = true;
                return true;
            }
            return false;
        }
        if (key == 'Y' && !shift) {
            if (redo()) {
                event.handled = true;
                return true;
            }
            return false;
        }
    }

    if (key == key::kBackspace || key == key::kDelete) {
        if (read_only_) {
            return false;
        }
        const EditCommitKind kind = control ? EditCommitKind::WordDelete : EditCommitKind::Delete;
        const EditSnapshot before = snapshot();
        return record_mutation(before,
                               key == key::kBackspace ? buffer_.delete_backward(control)
                                                      : buffer_.delete_forward(control),
                               kind);
    }
    if (key == key::kArrowLeft || key == key::kArrowRight) {
        return accept(key == key::kArrowLeft ? buffer_.move_left(control, shift)
                                             : buffer_.move_right(control, shift));
    }
    if (key == key::kHome || key == key::kEnd) {
        return accept(key == key::kHome ? buffer_.move_home(control, shift)
                                        : buffer_.move_end(control, shift));
    }
    if (control && key == 'A') {
        return accept(buffer_.select_all());
    }
    if (key == key::kEnter) {
        if (read_only_ || !multiline_) {
            return false;
        }
        const EditSnapshot before = snapshot();
        return record_mutation(before, buffer_.insert("\n"), EditCommitKind::Newline);
    }
    return false;
}

} // namespace tessera::ui::text
