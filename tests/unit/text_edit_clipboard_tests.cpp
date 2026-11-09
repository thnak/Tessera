// SPDX-License-Identifier: MIT
//
// Clipboard seam + edit controller cut/copy/paste (task-75): in-memory and unsupported backends,
// bounds, copy/cut/paste text+caret+selection outcomes, one undo step per cut/paste, read-only
// rules, chords/consume-only-on-change, emoji/ZWJ/multi-line/CRLF, truncation and determinism.

#include "tessera/hal/clipboard.hpp"
#include "tessera/ui/key_code.hpp"
#include "tessera/ui/text/edit_controller.hpp"
#include "tessera/ui/text/edit_history.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using tessera::hal::Clipboard;
using tessera::hal::ClipboardError;
using tessera::hal::create_memory_clipboard;
using tessera::hal::kMaxClipboardBytes;
using tessera::hal::MemoryClipboard;
using tessera::hal::UnsupportedClipboard;
using tessera::ui::Event;
using tessera::ui::EventType;
using tessera::ui::Modifier;
using tessera::ui::text::EditController;

namespace key = tessera::ui::key;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_edit_clipboard: " << message << "\n";
    }
    return condition;
}

Event text_event(std::uint32_t codepoint) {
    Event event;
    event.type = EventType::Text;
    event.text_codepoint = codepoint;
    return event;
}

Event key_event(std::uint32_t key_code, Modifier modifiers = Modifier::None) {
    Event event;
    event.type = EventType::KeyDown;
    event.key = key_code;
    event.modifiers = modifiers;
    return event;
}

bool feed(EditController& controller, Event event) {
    return controller.handle_event(event);
}

void type(EditController& controller, std::string_view text) {
    for (const char character : text) {
        feed(controller, text_event(static_cast<std::uint32_t>(character)));
    }
}

bool memory_roundtrip_and_bounds() {
    MemoryClipboard clipboard;
    if (!expect(clipboard.read_text().has_value() && clipboard.read_text()->empty(),
                "a fresh clipboard reads as empty")) {
        return false;
    }
    if (!expect(clipboard.write_text("hello").has_value(), "write succeeds") ||
        !expect(clipboard.read_text().has_value() && *clipboard.read_text() == "hello",
                "read round-trips") ||
        !expect(clipboard.write_text("").has_value() && clipboard.text().empty(),
                "empty write clears the clipboard")) {
        return false;
    }
    const std::string too_large(kMaxClipboardBytes + 1, 'x');
    const auto rejected = clipboard.write_text(too_large);
    if (!expect(!rejected.has_value() && rejected.error() == ClipboardError::TooLarge,
                "writes beyond the bound are rejected") ||
        !expect(clipboard.read_text().has_value() && clipboard.read_text()->empty(),
                "a rejected write leaves the clipboard unchanged")) {
        return false;
    }
    return expect(clipboard.write_text(std::string(kMaxClipboardBytes, 'y')).has_value(),
                  "the exact bound is accepted");
}

bool unsupported_backend() {
    UnsupportedClipboard clipboard;
    const auto read = clipboard.read_text();
    const auto wrote = clipboard.write_text("x");
    return expect(!read.has_value() && read.error() == ClipboardError::Unsupported,
                  "unsupported read is a value") &&
           expect(!wrote.has_value() && wrote.error() == ClipboardError::Unsupported,
                  "unsupported write is a value") &&
           expect(create_memory_clipboard() != nullptr, "memory factory");
}

bool copy_no_buffer_change() {
    EditController controller;
    MemoryClipboard clipboard;
    type(controller, "hello");
    controller.buffer().set_selection(1, 4);
    controller.clear_history();
    const tessera::ui::text::EditSnapshot before{std::string(controller.buffer().text()),
                                                 controller.buffer().caret(),
                                                 controller.buffer().anchor()};
    const auto result = controller.copy(clipboard);
    if (!expect(result.changed && result.bytes == 3 && result.error == ClipboardError::None,
                "copy reports three bytes") ||
        !expect(clipboard.text() == "ell", "copy wrote the selected text") ||
        !expect(controller.buffer().text() == before.text, "copy keeps the text") ||
        !expect(controller.buffer().caret() == before.caret &&
                    controller.buffer().anchor() == before.anchor,
                "copy keeps caret/selection") ||
        !expect(!controller.can_undo(), "copy records no history")) {
        return false;
    }
    controller.buffer().set_caret(0);
    return expect(!controller.copy(clipboard).changed, "copy without a selection is a no-op");
}

bool cut_one_undo_step() {
    EditController controller;
    MemoryClipboard clipboard;
    type(controller, "hello");
    controller.clear_history();
    controller.buffer().set_selection(1, 4);
    const auto result = controller.cut(clipboard);
    if (!expect(result.changed && result.bytes == 3, "cut copies the selection") ||
        !expect(clipboard.text() == "ell", "cut wrote the clipboard") ||
        !expect(controller.buffer().text() == "ho", "cut removed the selection") ||
        !expect(controller.buffer().caret() == 1 && !controller.buffer().has_selection(),
                "cut collapses the caret at the deletion point") ||
        !expect(controller.history().undo_entries() == 1, "cut is one undo step")) {
        return false;
    }
    if (!expect(controller.undo(), "undo the cut") ||
        !expect(controller.buffer().text() == "hello", "undo restored the cut text") ||
        !expect(controller.buffer().selection_start() == 1 &&
                    controller.buffer().selection_end() == 4,
                "undo restored the selection") ||
        !expect(controller.redo(), "redo the cut") ||
        !expect(controller.buffer().text() == "ho", "redo re-applied the cut")) {
        return false;
    }
    // Read-only blocks cut but still allows copy.
    controller.set_read_only(true);
    controller.buffer().set_selection(0, 1);
    const auto blocked = controller.cut(clipboard);
    return expect(!blocked.changed && controller.buffer().text() == "ho",
                  "read-only cut is a no-op") &&
           expect(controller.copy(clipboard).changed, "read-only copy still works");
}

bool paste_replaces_selection_one_step() {
    EditController controller;
    MemoryClipboard clipboard;
    (void)clipboard.write_text("X");
    type(controller, "abcd");
    controller.clear_history();
    controller.buffer().set_selection(1, 3);
    const auto result = controller.paste(clipboard);
    if (!expect(result.changed && result.bytes == 1 && !result.truncated, "paste changed") ||
        !expect(controller.buffer().text() == "aXd", "paste replaced the selection") ||
        !expect(controller.buffer().caret() == 2, "paste caret after the inserted text") ||
        !expect(controller.history().undo_entries() == 1, "paste is one undo step")) {
        return false;
    }
    if (!expect(controller.undo(), "undo paste") ||
        !expect(controller.buffer().text() == "abcd", "undo paste restored text") ||
        !expect(controller.buffer().selection_start() == 1 &&
                    controller.buffer().selection_end() == 3,
                "undo paste restored the selection")) {
        return false;
    }
    // Empty clipboard: no edit, no history step, chord unconsumed.
    MemoryClipboard empty;
    (void)clipboard.write_text("");
    const std::size_t steps_before_empty_paste = controller.history().undo_entries();
    const auto noop = controller.paste(empty);
    return expect(!noop.changed && controller.buffer().text() == "abcd",
                  "empty clipboard paste is a no-op") &&
           expect(controller.history().undo_entries() == steps_before_empty_paste,
                  "empty paste adds no step");
}

bool paste_bounds_and_crlf() {
    EditController controller(5);
    MemoryClipboard clipboard;
    (void)clipboard.write_text("abcdef");
    const auto result = controller.paste(clipboard);
    if (!expect(result.changed && result.truncated, "paste reports truncation at the buffer cap") ||
        !expect(controller.buffer().text().size() == 5, "paste truncates at the byte cap") ||
        !expect(controller.buffer().text() == "abcde", "paste keeps the leading bytes") ||
        !expect(controller.history().undo_entries() == 1, "truncated paste is one step")) {
        return false;
    }
    EditController crlf;
    MemoryClipboard crlf_clipboard;
    (void)crlf_clipboard.write_text("a\r\nb\rc");
    crlf.paste(crlf_clipboard);
    return expect(crlf.buffer().text() == "a\nb\nc", "paste normalizes CRLF/CR through the buffer");
}

bool unsupported_clipboard_is_noop() {
    EditController controller;
    type(controller, "hello");
    controller.buffer().set_selection(0, 5);
    UnsupportedClipboard clipboard;
    const auto cut_result = controller.cut(clipboard);
    if (!expect(!cut_result.changed && cut_result.error == ClipboardError::Unsupported,
                "cut reports unsupported and changes nothing") ||
        !expect(controller.buffer().text() == "hello", "unsupported cut keeps the text")) {
        return false;
    }
    const auto paste_result = controller.paste(clipboard);
    return expect(!paste_result.changed && paste_result.error == ClipboardError::Unsupported,
                  "paste reports unsupported and changes nothing");
}

bool chords_and_read_only() {
    EditController controller;
    MemoryClipboard clipboard;
    controller.set_clipboard(&clipboard);
    type(controller, "hello");
    controller.buffer().set_selection(0, 5);
    Event copy = key_event('C', Modifier::Control);
    if (!expect(controller.handle_event(copy), "Ctrl+C consumed") ||
        !expect(copy.handled, "Ctrl+C handled") ||
        !expect(clipboard.text() == "hello", "Ctrl+C copied")) {
        return false;
    }
    controller.buffer().set_caret(0);
    Event copy_empty = key_event('C', Modifier::Control);
    if (!expect(!controller.handle_event(copy_empty), "Ctrl+C without a selection is unconsumed") ||
        !expect(!copy_empty.handled, "inert Ctrl+C is not handled")) {
        return false;
    }
    controller.buffer().set_selection(1, 3);
    Event cut = key_event('X', Modifier::Control);
    if (!expect(controller.handle_event(cut), "Ctrl+X consumed") ||
        !expect(cut.handled, "Ctrl+X handled") ||
        !expect(controller.buffer().text() == "hlo", "Ctrl+X cut") ||
        !expect(clipboard.text() == "el", "Ctrl+X copied the range")) {
        return false;
    }
    Event paste = key_event('V', Modifier::Control);
    if (!expect(controller.handle_event(paste), "Ctrl+V consumed") ||
        !expect(paste.handled, "Ctrl+V handled") ||
        !expect(controller.buffer().text() == "hello", "Ctrl+V pasted")) {
        return false;
    }
    // Read-only: copy works, cut/paste chords are not consumed.
    controller.set_read_only(true);
    controller.buffer().set_selection(0, 2);
    Event copy_ro = key_event('C', Modifier::Control);
    Event cut_ro = key_event('X', Modifier::Control);
    Event paste_ro = key_event('V', Modifier::Control);
    if (!expect(controller.handle_event(copy_ro) && copy_ro.handled, "read-only copy works") ||
        !expect(!controller.handle_event(cut_ro) && !cut_ro.handled,
                "read-only Ctrl+X is unconsumed") ||
        !expect(!controller.handle_event(paste_ro) && !paste_ro.handled,
                "read-only Ctrl+V is unconsumed") ||
        !expect(controller.buffer().text() == "hello", "read-only chords keep the text")) {
        return false;
    }
    controller.set_read_only(false);
    // prevent_default and Alt chords are respected.
    Event prevented = key_event('V', Modifier::Control);
    prevented.prevent_default = true;
    if (!expect(!controller.handle_event(prevented), "prevent_default suppresses Ctrl+V")) {
        return false;
    }
    Event alt = key_event('V', Modifier::Control | Modifier::Alt);
    if (!expect(!controller.handle_event(alt), "Alt+Ctrl+V is not a clipboard chord")) {
        return false;
    }
    // Without an injected clipboard the chords are inert.
    EditController detached;
    MemoryClipboard unused;
    type(detached, "x");
    detached.buffer().set_selection(0, 1);
    Event no_backend = key_event('C', Modifier::Control);
    return expect(!detached.handle_event(no_backend), "no clipboard backend means inert chords") &&
           expect(!no_backend.handled, "inert chord not handled");
}

bool unicode_and_multiline() {
    EditController controller;
    MemoryClipboard clipboard;
    // Multi-line content with emoji and a ZWJ sequence.
    type(controller, "a");
    controller.set_multiline(true);
    feed(controller, key_event(key::kEnter));
    type(controller, "b");
    controller.buffer().set_caret(0);
    (void)clipboard.write_text("\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB\n\xF0\x9F\x98\x80");
    const auto result = controller.paste(clipboard);
    if (!expect(result.changed && !result.truncated, "unicode paste changed") ||
        !expect(controller.buffer().text() ==
                    "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB\n\xF0\x9F\x98\x80"
                    "a\nb",
                "unicode + ZWJ + multiline paste text") ||
        !expect(
            controller.buffer().caret() ==
                std::string_view("\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB\n\xF0\x9F\x98\x80")
                    .size(),
            "caret after the pasted graphemes")) {
        return false;
    }
    // Copy the multi-line selection and round-trip it through the clipboard.
    controller.buffer().set_selection(0, controller.buffer().text().size());
    const auto copied = controller.copy(clipboard);
    return expect(copied.changed && clipboard.text() == controller.buffer().text(),
                  "copy round-trips multi-line UTF-8");
}

bool determinism() {
    const auto run = [] {
        EditController controller;
        MemoryClipboard clipboard;
        controller.set_clipboard(&clipboard);
        std::vector<std::string> texts;
        std::vector<std::string> clipboards;
        for (int index = 0; index < 24; ++index) {
            switch (index % 6) {
            case 0:
                type(controller, "ab");
                break;
            case 1:
                controller.buffer().set_selection(0, 1);
                feed(controller, key_event('C', Modifier::Control));
                break;
            case 2:
                controller.buffer().set_selection(0, controller.buffer().text().size());
                feed(controller, key_event('X', Modifier::Control));
                break;
            case 3:
                feed(controller, key_event('V', Modifier::Control));
                break;
            case 4:
                feed(controller, key_event('Z', Modifier::Control));
                break;
            default:
                feed(controller, key_event('V', Modifier::Control | Modifier::Shift));
                break;
            }
            texts.emplace_back(controller.buffer().text());
            clipboards.push_back(clipboard.text());
        }
        return std::pair{texts, clipboards};
    };
    return expect(run() == run(), "the same clipboard script is deterministic");
}

} // namespace

int main() {
    if (!memory_roundtrip_and_bounds()) {
        return 1;
    }
    if (!unsupported_backend()) {
        return 2;
    }
    if (!copy_no_buffer_change()) {
        return 3;
    }
    if (!cut_one_undo_step()) {
        return 4;
    }
    if (!paste_replaces_selection_one_step()) {
        return 5;
    }
    if (!paste_bounds_and_crlf()) {
        return 6;
    }
    if (!unsupported_clipboard_is_noop()) {
        return 7;
    }
    if (!chords_and_read_only()) {
        return 8;
    }
    if (!unicode_and_multiline()) {
        return 9;
    }
    if (!determinism()) {
        return 10;
    }

    std::cout << "text_edit_clipboard: ok\n";
    return 0;
}
