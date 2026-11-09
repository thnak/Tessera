// SPDX-License-Identifier: MIT
//
// EditController undo/redo integration tests (task-69): type -> undo -> redo, coalescing across
// caret moves, non-mergeable kinds, the set_text boundary, selection/caret restore, chord
// semantics/read-only, grapheme cases and determinism.

#include "tessera/ui/key_code.hpp"
#include "tessera/ui/text/edit_controller.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::ui::Event;
using tessera::ui::EventType;
using tessera::ui::Modifier;
using tessera::ui::text::EditController;

namespace key = tessera::ui::key;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_undo: " << message << "\n";
    }
    return condition;
}

Event text_event(std::uint32_t codepoint) {
    Event event;
    event.type = EventType::Text;
    event.text_codepoint = codepoint;
    return event;
}

Event edit_event(const std::string& text) {
    Event event;
    event.type = EventType::EditText;
    const std::size_t size = std::min<std::size_t>(text.size(), event.text.size());
    std::copy_n(text.data(), size, event.text.begin());
    event.text_size = static_cast<std::uint32_t>(size);
    return event;
}

Event key_event(std::uint32_t key_code, Modifier modifiers = Modifier::None) {
    Event event;
    event.type = EventType::KeyDown;
    event.key = key_code;
    event.modifiers = modifiers;
    return event;
}

// handle_event takes an Event& (it may mark the source event handled), so feed temporaries through
// a named parameter instead of binding them to a non-const reference.
bool feed(EditController& controller, Event event) {
    return controller.handle_event(event);
}

bool controller_type_undo_redo() {
    EditController controller;
    controller.tick(0.0);
    for (const char character : std::string("abc")) {
        Event event = text_event(static_cast<std::uint32_t>(character));
        controller.tick(0.1);
        if (!expect(feed(controller, event), "typing consumed")) {
            return false;
        }
    }
    if (!expect(controller.buffer().text() == "abc", "typed text") ||
        !expect(controller.history().undo_entries() == 1, "plain typing coalesced into one step") ||
        !expect(controller.can_undo() && !controller.can_redo(), "one undo step, no redo")) {
        return false;
    }
    if (!expect(controller.undo(), "undo consumed") ||
        !expect(controller.buffer().text().empty(), "undo emptied the buffer") ||
        !expect(controller.buffer().caret() == 0 && controller.buffer().anchor() == 0,
                "undo restored the caret/selection") ||
        !expect(controller.can_redo(), "undo enabled redo") ||
        !expect(controller.buffer().text().empty() && controller.activity_count() > 0,
                "undo is activity")) {
        return false;
    }
    if (!expect(controller.redo(), "redo consumed") ||
        !expect(controller.buffer().text() == "abc", "redo restored the text") ||
        !expect(controller.buffer().caret() == 3, "redo restored the caret") ||
        !expect(!controller.can_redo(), "redo drained")) {
        return false;
    }
    if (!expect(controller.undo(), "undo again") ||
        !expect(controller.buffer().text().empty(), "undo again emptied")) {
        return false;
    }
    // A new edit after undo must truncate the redo tail.
    controller.tick(2.0);
    Event fresh = text_event('X');
    if (!expect(feed(controller, fresh), "new edit consumed") ||
        !expect(controller.buffer().text() == "X", "new edit text") ||
        !expect(!controller.can_redo(), "new edit truncated the redo tail")) {
        return false;
    }
    return true;
}

bool controller_coalescing_boundary() {
    EditController controller;
    controller.tick(0.0);
    feed(controller, text_event('a'));
    feed(controller, text_event('b'));
    if (!expect(controller.history().undo_entries() == 1, "quick typing merges")) {
        return false;
    }
    controller.tick(0.6);
    feed(controller, text_event('c'));
    if (!expect(controller.history().undo_entries() == 2, "typing after the window splits")) {
        return false;
    }
    // A caret move ends the run even inside the window.
    Event left = key_event(key::kArrowLeft);
    controller.tick(0.7);
    feed(controller, left);
    controller.tick(0.75);
    feed(controller, text_event('d'));
    if (!expect(controller.history().undo_entries() == 3, "a caret move breaks the run")) {
        return false;
    }
    return expect(controller.buffer().text() == "abdc", "typed text with a caret move");
}

bool controller_kinds_and_set_text() {
    EditController controller;
    controller.tick(0.0);
    feed(controller, text_event('a'));
    feed(controller, text_event('b'));
    controller.tick(0.1);
    feed(controller, key_event(key::kEnter)); // single-line: ignored
    if (!expect(controller.history().undo_entries() == 1, "ignored Enter records nothing")) {
        return false;
    }
    controller.set_multiline(true);
    controller.tick(0.2);
    if (!expect(feed(controller, key_event(key::kEnter)), "multiline Enter consumed") ||
        !expect(controller.history().undo_entries() == 2, "newline starts a new step")) {
        return false;
    }
    controller.tick(0.3);
    feed(controller, text_event('c'));
    controller.tick(0.4);
    // Word delete never merges.
    if (!expect(feed(controller, key_event(key::kBackspace, Modifier::Control)),
                "word delete consumed") ||
        !expect(controller.history().undo_entries() == 4, "word delete starts a new step")) {
        return false;
    }
    // A paste/IME EditText commit never merges.
    controller.tick(0.5);
    if (!expect(feed(controller, edit_event("XY")), "paste consumed") ||
        !expect(controller.history().undo_entries() == 5, "paste starts a new step")) {
        return false;
    }
    // set_text is a clean boundary: it neither records nor keeps the old history.
    controller.set_text("replaced");
    if (!expect(!controller.can_undo(), "set_text cleared the history") ||
        !expect(controller.buffer().text() == "replaced", "set_text applied")) {
        return false;
    }
    controller.tick(0.6);
    feed(controller, text_event('!'));
    if (!expect(controller.history().undo_entries() == 1, "typing after set_text starts fresh") ||
        !expect(controller.undo(), "undo after set_text") ||
        !expect(controller.buffer().text() == "replaced",
                "undo stops at the controlled-value boundary")) {
        return false;
    }
    return true;
}

bool controller_selection_restore() {
    EditController controller;
    controller.tick(0.0);
    for (const char character : std::string("abcd")) {
        feed(controller, text_event(static_cast<std::uint32_t>(character)));
        controller.tick(0.1);
    }
    // Select [1,3) with Shift+Left twice from the end.
    feed(controller, key_event(key::kArrowLeft, Modifier::Shift));
    feed(controller, key_event(key::kArrowLeft, Modifier::Shift));
    if (!expect(controller.buffer().selection_start() == 2 &&
                    controller.buffer().selection_end() == 4,
                "selection prepared")) {
        return false;
    }
    controller.tick(1.0);
    // Replacing the selection always starts a new step, and its before-snapshot keeps the range.
    if (!expect(feed(controller, text_event('X')), "selection replace consumed")) {
        return false;
    }
    if (!expect(controller.undo(), "undo the selection replace") ||
        !expect(controller.buffer().text() == "abcd", "selection text restored") ||
        !expect(controller.buffer().selection_start() == 2 &&
                    controller.buffer().selection_end() == 4,
                "selection range restored") ||
        !expect(controller.buffer().caret() == 2, "caret restored to the selection focus")) {
        return false;
    }
    return true;
}

bool chords_and_read_only() {
    EditController controller;
    controller.tick(0.0);
    feed(controller, text_event('a'));
    controller.tick(0.1);
    feed(controller, text_event('b'));
    // Ctrl+Z undoes and marks handled.
    Event undo_chord = key_event('Z', Modifier::Control);
    if (!expect(controller.handle_event(undo_chord), "Ctrl+Z consumed") ||
        !expect(undo_chord.handled, "Ctrl+Z marks handled") ||
        !expect(controller.buffer().text().empty(), "Ctrl+Z undid")) {
        return false;
    }
    // Ctrl+Shift+Z redoes.
    Event redo_chord = key_event('Z', Modifier::Control | Modifier::Shift);
    if (!expect(controller.handle_event(redo_chord), "Ctrl+Shift+Z consumed") ||
        !expect(redo_chord.handled, "Ctrl+Shift+Z marks handled") ||
        !expect(controller.buffer().text() == "ab", "Ctrl+Shift+Z redid")) {
        return false;
    }
    // Ctrl+Y also redoes (after an undo).
    feed(controller, key_event('Z', Modifier::Control));
    Event redo_y = key_event('Y', Modifier::Control);
    if (!expect(controller.handle_event(redo_y), "Ctrl+Y consumed") ||
        !expect(redo_y.handled, "Ctrl+Y marks handled") ||
        !expect(controller.buffer().text() == "ab", "Ctrl+Y redid")) {
        return false;
    }
    // Empty stacks: a fresh controller's chord is not consumed and not marked handled.
    EditController empty_controller;
    Event nothing = key_event('Z', Modifier::Control);
    if (!expect(!empty_controller.handle_event(nothing),
                "Ctrl+Z with an empty stack is not consumed") ||
        !expect(!nothing.handled, "an inert chord is not marked handled")) {
        return false;
    }
    // Alt+Ctrl+Z is not an undo chord. Keep an undo entry available for the read-only check.
    controller.tick(1.0);
    feed(controller, text_event('Q'));
    Event alt = key_event('Z', Modifier::Control | Modifier::Alt);
    if (!expect(!controller.handle_event(alt), "Alt+Ctrl+Z is not consumed") ||
        !expect(!alt.handled, "Alt+Ctrl+Z is not handled")) {
        return false;
    }
    // prevent_default suppresses the chord.
    Event prevented = key_event('Z', Modifier::Control);
    prevented.prevent_default = true;
    if (!expect(!controller.handle_event(prevented), "prevent_default suppresses undo")) {
        return false;
    }
    // Read-only ignores both commands and leaves the history intact.
    controller.set_read_only(true);
    Event read_only = key_event('Z', Modifier::Control);
    if (!expect(!controller.handle_event(read_only), "read-only ignores Ctrl+Z") ||
        !expect(!read_only.handled, "read-only does not mark handled") ||
        !expect(controller.can_undo(), "read-only keeps the history")) {
        return false;
    }
    controller.set_read_only(false);
    return expect(feed(controller, key_event('Z', Modifier::Control)),
                  "undo works again after read-only");
}

bool emoji_and_graphemes() {
    EditController controller;
    controller.tick(0.0);
    // A single emoji code point is plain typing and coalesces with the run.
    Event emoji = text_event(0x1F600U);
    feed(controller, emoji);
    feed(controller, text_event('a'));
    if (!expect(controller.buffer().text() == "\xF0\x9F\x98\x80"
                                              "a",
                "emoji text") ||
        !expect(controller.history().undo_entries() == 1, "emoji typing coalesces")) {
        return false;
    }
    controller.tick(0.1);
    // A ZWJ grapheme commit (paste/IME path) is one non-mergeable step.
    Event zwj = edit_event("\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB");
    if (!expect(feed(controller, zwj), "ZWJ commit consumed") ||
        !expect(controller.history().undo_entries() == 2, "ZWJ commit starts a new step")) {
        return false;
    }
    controller.tick(0.2);
    if (!expect(controller.undo(), "undo ZWJ") ||
        !expect(controller.buffer().text() == "\xF0\x9F\x98\x80"
                                              "a",
                "ZWJ undone exactly") ||
        !expect(controller.buffer().caret() == controller.buffer().text().size(),
                "caret after ZWJ undo")) {
        return false;
    }
    controller.tick(10.0);
    // A grapheme-boundary delete step (e.g. backspace over the emoji) undoes exactly one grapheme.
    feed(controller, key_event(key::kBackspace));
    if (!expect(controller.buffer().text() == "\xF0\x9F\x98\x80",
                "backspace removed the trailing grapheme")) {
        return false;
    }
    return expect(controller.undo(), "undo grapheme delete") &&
           expect(controller.buffer().text() == "\xF0\x9F\x98\x80"
                                                "a",
                  "grapheme delete undone exactly");
}

bool determinism() {
    const auto run = [] {
        EditController controller;
        std::vector<std::string> texts;
        std::vector<std::pair<std::size_t, std::size_t>> carets;
        for (int index = 0; index < 40; ++index) {
            controller.tick(static_cast<double>(index) * 0.15);
            if (index % 7 == 3) {
                feed(controller, key_event('Z', Modifier::Control));
            } else if (index % 11 == 5) {
                feed(controller, key_event('Z', Modifier::Control | Modifier::Shift));
            } else if (index % 5 == 0) {
                feed(controller, key_event(key::kBackspace));
            } else {
                feed(controller, text_event(static_cast<std::uint32_t>('a' + index % 26)));
            }
            texts.emplace_back(controller.buffer().text());
            carets.emplace_back(controller.buffer().caret(), controller.buffer().anchor());
        }
        return std::pair{texts, carets};
    };
    return expect(run() == run(), "the same script gives identical text/caret/selection results");
}

} // namespace

int main() {
    if (!controller_type_undo_redo()) {
        return 1;
    }
    if (!controller_coalescing_boundary()) {
        return 2;
    }
    if (!controller_kinds_and_set_text()) {
        return 3;
    }
    if (!controller_selection_restore()) {
        return 4;
    }
    if (!chords_and_read_only()) {
        return 5;
    }
    if (!emoji_and_graphemes()) {
        return 6;
    }
    if (!determinism()) {
        return 7;
    }

    std::cout << "text_undo: ok\n";
    return 0;
}
