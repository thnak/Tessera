// SPDX-License-Identifier: MIT
//
// EditController: normalized Text/IME/composition events and editing keys into one EditBuffer,
// prevent_default suppression, read-only behavior, composition commit/cancel, and the bounded
// length semantics.

#include "tessera/ui/key_code.hpp"
#include "tessera/ui/scene/event.hpp"
#include "tessera/ui/text/edit_controller.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::ui::Event;
using tessera::ui::EventType;
using tessera::ui::Modifier;
using tessera::ui::text::EditController;
using tessera::ui::text::EditStatus;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_edit_controller: " << message << "\n";
    }
    return condition;
}

Event text_event(std::uint32_t codepoint) {
    Event event;
    event.type = EventType::Text;
    event.text_codepoint = codepoint;
    return event;
}

Event key_event(std::uint32_t key, Modifier modifiers = Modifier::None) {
    Event event;
    event.type = EventType::KeyDown;
    event.key = key;
    event.modifiers = modifiers;
    return event;
}

// Forwards a temporary event to the controller (the controller marks consumed events handled).
bool apply(EditController& controller, Event event) {
    return controller.handle_event(event);
}

Event payload_event(EventType type, std::string_view text) {
    Event event;
    event.type = type;
    const std::size_t size = std::min<std::size_t>(text.size(), event.text.size());
    std::copy_n(text.data(), size, event.text.begin());
    event.text_size = static_cast<std::uint32_t>(size);
    return event;
}

bool text_and_suppression() {
    EditController controller;
    Event insert = text_event('a');
    if (!expect(controller.handle_event(insert), "text event consumed") ||
        !expect(insert.handled, "consumed text event is marked handled") ||
        !expect(controller.buffer().text() == "a", "text inserted") ||
        !expect(controller.take_changed(), "change reported once") ||
        !expect(!controller.take_changed(), "change flag resets")) {
        return false;
    }

    Event suppressed = text_event('b');
    suppressed.prevent_default = true;
    if (!expect(!controller.handle_event(suppressed), "prevent_default suppresses the editor") ||
        !expect(controller.buffer().text() == "a", "suppressed text is not inserted") ||
        !expect(!suppressed.handled, "suppressed event is not marked handled")) {
        return false;
    }

    Event prehandled = text_event('c');
    prehandled.handled = true;
    return expect(!controller.handle_event(prehandled), "already-handled event is ignored") &&
           expect(controller.buffer().text() == "a", "prehandled text is not inserted");
}

bool read_only_behavior() {
    EditController controller;
    controller.set_text("abc");
    controller.set_read_only(true);
    if (!expect(!apply(controller, text_event('d')), "read-only text ignored") ||
        !expect(controller.buffer().text() == "abc", "read-only text unchanged") ||
        !expect(!apply(controller, key_event(tessera::ui::key::kBackspace)),
                "read-only backspace ignored")) {
        return false;
    }
    Event left = key_event(tessera::ui::key::kArrowLeft);
    return expect(controller.handle_event(left), "read-only still consumes navigation") &&
           expect(controller.buffer().caret() == 2, "navigation moved the caret");
}

bool editing_keys() {
    EditController controller;
    controller.set_text("abc");
    Event backspace = key_event(tessera::ui::key::kBackspace);
    if (!expect(controller.handle_event(backspace), "backspace consumed") ||
        !expect(controller.buffer().text() == "ab", "backspace deletes a cluster")) {
        return false;
    }
    Event home = key_event(tessera::ui::key::kHome);
    Event del = key_event(tessera::ui::key::kDelete);
    if (!expect(controller.handle_event(home), "home consumed") ||
        !expect(controller.handle_event(del), "delete consumed") ||
        !expect(controller.buffer().text() == "b", "forward delete")) {
        return false;
    }
    EditController shift;
    shift.set_text("abc");
    Event left = key_event(tessera::ui::key::kArrowLeft);
    Event shift_right = key_event(tessera::ui::key::kArrowRight, Modifier::Shift);
    return expect(shift.handle_event(left) && shift.handle_event(shift_right),
                  "shift+right consumed") &&
           expect(shift.buffer().caret() == 3 && shift.buffer().anchor() == 2,
                  "shift extends the selection");
}

bool word_keys_and_select_all() {
    EditController controller;
    controller.set_text("hello world");
    Event word_backspace = key_event(tessera::ui::key::kBackspace, Modifier::Control);
    if (!expect(controller.handle_event(word_backspace), "ctrl+backspace consumed") ||
        !expect(controller.buffer().text() == "hello ", "ctrl+backspace deletes a word")) {
        return false;
    }
    Event select_all = key_event('A', Modifier::Control);
    if (!expect(controller.handle_event(select_all), "ctrl+a consumed") ||
        !expect(controller.buffer().has_selection(), "ctrl+a selects all")) {
        return false;
    }
    Event replacement = text_event('x');
    return expect(controller.handle_event(replacement), "text replaces the selection") &&
           expect(controller.buffer().text() == "x", "selection replaced");
}

bool enter_behavior() {
    EditController single;
    single.set_text("ab");
    Event enter = key_event(tessera::ui::key::kEnter);
    if (!expect(!single.handle_event(enter), "single-line enter is not consumed") ||
        !expect(single.buffer().text() == "ab", "single-line enter inserts nothing")) {
        return false;
    }
    EditController multi;
    multi.set_multiline(true);
    multi.set_text("ab");
    Event multi_enter = key_event(tessera::ui::key::kEnter);
    return expect(multi.handle_event(multi_enter), "multiline enter consumed") &&
           expect(multi.buffer().text() == "ab\n", "multiline enter inserts a newline");
}

bool composition_commit() {
    EditController controller;
    controller.set_text("hello world");
    controller.buffer().set_selection(6, 11);

    Event start = payload_event(EventType::CompositionStart, {});
    if (!expect(controller.handle_event(start), "composition start consumed") ||
        !expect(controller.composing(), "composing flag") ||
        !expect(controller.composition_start() == 6 && controller.composition_end() == 11,
                "composition range is the selection")) {
        return false;
    }
    Event update = payload_event(EventType::CompositionUpdate, "preedit");
    if (!expect(controller.handle_event(update), "composition update consumed") ||
        !expect(controller.preedit() == "preedit", "preedit stored") ||
        !expect(controller.buffer().text() == "hello world",
                "preedit is not in the buffer until commit")) {
        return false;
    }
    Event commit = payload_event(EventType::EditText, "there");
    if (!expect(controller.handle_event(commit), "commit consumed") ||
        !expect(!controller.composing(), "composition ended on commit") ||
        !expect(controller.preedit().empty(), "preedit cleared") ||
        !expect(controller.buffer().text() == "hello there", "commit replaced the range") ||
        !expect(controller.buffer().caret() == 11, "caret after the commit")) {
        return false;
    }
    Event end = payload_event(EventType::CompositionEnd, {});
    return expect(!controller.handle_event(end), "end after commit is a no-op");
}

bool composition_cancel_and_invalid() {
    EditController controller;
    controller.set_text("abc");
    controller.buffer().set_caret(1);
    if (!expect(apply(controller, payload_event(EventType::CompositionStart, {})),
                "start consumed")) {
        return false;
    }
    Event invalid = payload_event(EventType::CompositionUpdate, "\x80");
    if (!expect(!controller.handle_event(invalid), "invalid preedit rejected") ||
        !expect(controller.preedit().empty(), "preedit unchanged by invalid input")) {
        return false;
    }
    if (!expect(apply(controller, payload_event(EventType::CompositionUpdate, "xy")),
                "valid preedit consumed") ||
        !expect(controller.preedit() == "xy", "preedit stored for cancel test") ||
        !expect(apply(controller, payload_event(EventType::CompositionEnd, {})),
                "cancel consumed") ||
        !expect(!controller.composing(), "cancel ends composition") ||
        !expect(controller.buffer().text() == "abc", "cancel restores the buffer") ||
        !expect(controller.buffer().caret() == 1, "cancel keeps the caret")) {
        return false;
    }
    // A result without a composition start inserts at the caret.
    Event result = payload_event(EventType::EditText, "Z");
    return expect(controller.handle_event(result), "stray result consumed") &&
           expect(controller.buffer().text() == "aZbc", "stray result inserted at the caret");
}

bool bounded_length() {
    EditController controller(4);
    for (int index = 0; index < 4; ++index) {
        if (!expect(apply(controller, text_event('a')), "text accepted")) {
            return false;
        }
    }
    if (!expect(controller.buffer().text() == "aaaa", "buffer filled to the bound")) {
        return false;
    }
    Event overflow = text_event('b');
    if (!expect(controller.handle_event(overflow), "over-limit text is still consumed") ||
        !expect(controller.buffer().text() == "aaaa", "over-limit text is not inserted") ||
        !expect(controller.last_result().status == EditStatus::Truncated,
                "over-limit status is truncated")) {
        return false;
    }
    return expect(controller.buffer().size() == 4, "bound never exceeded");
}

bool controlled_value_sync() {
    EditController controller;
    if (!expect(apply(controller, payload_event(EventType::CompositionStart, {})),
                "start consumed")) {
        return false;
    }
    controller.set_text("fresh");
    return expect(!controller.composing(), "set_text cancels composition") &&
           expect(controller.preedit().empty(), "set_text clears the preedit") &&
           expect(controller.buffer().text() == "fresh", "controlled value applied") &&
           expect(controller.buffer().caret() == 5, "controlled value caret at end");
}

bool determinism() {
    const auto run = [](EditController& controller) {
        controller.set_text("hello world");
        controller.buffer().move_end(true, false);
        apply(controller, key_event(tessera::ui::key::kBackspace, Modifier::Control));
        apply(controller, payload_event(EventType::CompositionStart, {}));
        apply(controller, payload_event(EventType::CompositionUpdate, "pre"));
        apply(controller, payload_event(EventType::EditText, "committed"));
        apply(controller, key_event(tessera::ui::key::kArrowLeft, Modifier::Shift));
    };
    EditController first;
    EditController second;
    run(first);
    run(second);
    return expect(first.buffer().text() == second.buffer().text(), "same final text") &&
           expect(first.buffer().caret() == second.buffer().caret(), "same caret") &&
           expect(first.buffer().anchor() == second.buffer().anchor(), "same anchor") &&
           expect(first.buffer().text() == "hello committed", "script result");
}

} // namespace

int main() {
    if (!text_and_suppression()) {
        return 1;
    }
    if (!read_only_behavior()) {
        return 2;
    }
    if (!editing_keys()) {
        return 3;
    }
    if (!word_keys_and_select_all()) {
        return 4;
    }
    if (!enter_behavior()) {
        return 5;
    }
    if (!composition_commit()) {
        return 6;
    }
    if (!composition_cancel_and_invalid()) {
        return 7;
    }
    if (!bounded_length()) {
        return 8;
    }
    if (!controlled_value_sync()) {
        return 9;
    }
    if (!determinism()) {
        return 10;
    }

    std::cout << "text_edit_controller: ok\n";
    return 0;
}
