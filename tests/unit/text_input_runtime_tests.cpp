// SPDX-License-Identifier: MIT
//
// TextInputRuntime: one TextChange scene event per committed edit, none for preedit/repeat-only
// events, the typed handler adapter, focus/read-only gating, route ordering, route-log equality
// when no handler is bound, the borrowed full-value view, and determinism.

#include "tessera/ui/key_code.hpp"
#include "tessera/ui/scene/event_payloads.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/text/edit_controller.hpp"
#include "tessera/ui/text/text_input_runtime.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::ui::Event;
using tessera::ui::EventPhase;
using tessera::ui::EventType;
using tessera::ui::InputEventType;
using tessera::ui::Modifier;
using tessera::ui::Node;
using tessera::ui::style::PseudoState;
using tessera::ui::text::EditController;
namespace key = tessera::ui::key;
using tessera::ui::TextChangeBinding;
using tessera::ui::TextChangeEvent;
using tessera::ui::text::TextInputRuntime;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_input_runtime: " << message << "\n";
    }
    return condition;
}

struct ChangeSink {
    std::string value;
    std::uint32_t selection_start = 0;
    std::uint32_t selection_end = 0;
    int calls = 0;
};

void on_change(void* context, const TextChangeEvent& event) {
    auto* sink = static_cast<ChangeSink*>(context);
    sink->value.assign(event.value.data(), event.value.size());
    sink->selection_start = event.selection_start;
    sink->selection_end = event.selection_end;
    ++sink->calls;
}

struct Recorder final : Node {
    using Node::Node;

    struct Entry {
        EventType type = EventType::PointerDown;
        EventPhase phase = EventPhase::Target;

        bool operator==(const Entry&) const = default;
    };

    void on_event(Event& event) override {
        log.push_back({event.type, event.phase});
    }

    std::vector<Entry> log;
};

struct Fixture {
    explicit Fixture(bool bind_handler = true, bool focus = true) {
        root.set_bounds({0, 0, 200, 200});
        auto owner = std::make_unique<Node>(2);
        input = owner.get();
        input->set_bounds({0, 0, 100, 24});
        input->set_focusable(true);
        if (focus) {
            input->set_pseudo_state(PseudoState::Focus, true);
        }
        root.add_child(std::move(owner));
        runtime.attach(input);
        if (bind_handler) {
            tessera::ui::bind_text_change(*input, binding);
        }
    }

    [[nodiscard]] std::size_t count_log(EventType type) const {
        std::size_t count = 0;
        for (const Recorder::Entry& entry : root.log) {
            if (entry.type == type) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::vector<Recorder::Entry> log_without(EventType skip) const {
        std::vector<Recorder::Entry> filtered;
        for (const Recorder::Entry& entry : root.log) {
            if (entry.type != skip) {
                filtered.push_back(entry);
            }
        }
        return filtered;
    }

    Recorder root{1};
    Node* input = nullptr;
    EditController controller;
    TextInputRuntime runtime{controller};
    ChangeSink sink;
    TextChangeBinding binding{&on_change, &sink};
};

Event text_event(std::uint32_t codepoint) {
    Event event;
    event.type = EventType::Text;
    event.text_codepoint = codepoint;
    return event;
}

Event key_event(std::uint32_t key, Modifier modifiers = Modifier::None, bool repeat = false) {
    Event event;
    event.type = EventType::KeyDown;
    event.key = key;
    event.modifiers = modifiers;
    event.repeat = repeat;
    return event;
}

Event payload_event(EventType type, std::string_view text) {
    Event event;
    event.type = type;
    const std::size_t size = std::min<std::size_t>(text.size(), event.text.size());
    std::copy_n(text.data(), size, event.text.begin());
    event.text_size = static_cast<std::uint32_t>(size);
    return event;
}

bool insert_commit_once() {
    Fixture fixture;
    Event insert = text_event('A');
    if (!expect(fixture.runtime.handle_event(insert), "text event consumed") ||
        !expect(insert.handled, "consumed source event is marked handled") ||
        !expect(fixture.runtime.change_events() == 1, "one change event per insertion") ||
        !expect(fixture.sink.calls == 1, "the typed adapter ran once") ||
        !expect(fixture.sink.value == "A", "payload value is the new text") ||
        !expect(fixture.sink.selection_start == 1 && fixture.sink.selection_end == 1,
                "payload selection is the half-open caret byte offset") ||
        !expect(fixture.runtime.last_change_handled(), "a bound change handler marks handled")) {
        return false;
    }
    // Route: the change event reaches the root through capture then bubble.
    if (!expect(fixture.count_log(EventType::TextChange) == 2,
                "change event visits the root in capture and bubble")) {
        return false;
    }
    const auto& log = fixture.root.log;
    std::size_t first = log.size();
    std::size_t last = 0;
    for (std::size_t index = 0; index < log.size(); ++index) {
        if (log[index].type == EventType::TextChange) {
            first = std::min(first, index);
            last = std::max(last, index);
        }
    }
    return expect(log[first].phase == EventPhase::Capture, "capture phase first") &&
           expect(log[last].phase == EventPhase::Bubble, "bubble phase last");
}

bool one_change_per_edit() {
    Fixture fixture;
    for (const char character : std::string_view("ABC")) {
        Event event = text_event(static_cast<std::uint32_t>(character));
        if (!fixture.runtime.handle_event(event)) {
            return false;
        }
    }
    return expect(fixture.runtime.change_events() == 3, "three insertions, three changes") &&
           expect(fixture.sink.calls == 3, "adapter called per edit") &&
           expect(fixture.sink.value == "ABC", "last payload is the full value") &&
           expect(fixture.sink.selection_start == 3 && fixture.sink.selection_end == 3,
                  "last payload caret offset");
}

bool composition_commit_once() {
    Fixture fixture;
    Event start = payload_event(EventType::CompositionStart, {});
    Event update = payload_event(EventType::CompositionUpdate, "にほ");
    if (!expect(fixture.runtime.handle_event(start), "composition start consumed") ||
        !expect(fixture.runtime.handle_event(update), "composition update consumed") ||
        !expect(fixture.runtime.change_events() == 0,
                "preedit updates do not emit a change event") ||
        !expect(fixture.sink.calls == 0, "adapter silent during preedit")) {
        return false;
    }
    Event commit = payload_event(EventType::EditText, "日本");
    if (!expect(fixture.runtime.handle_event(commit), "commit consumed") ||
        !expect(fixture.runtime.change_events() == 1, "one change event for the commit") ||
        !expect(fixture.sink.value == "日本", "committed text is the payload value") ||
        !expect(fixture.sink.selection_start == 6 && fixture.sink.selection_end == 6,
                "selection is UTF-8 byte offsets (3 bytes per code point)")) {
        return false;
    }
    Event end = payload_event(EventType::CompositionEnd, {});
    return expect(!fixture.runtime.handle_event(end), "end after commit is a no-op") &&
           expect(fixture.runtime.change_events() == 1, "no extra change event on end");
}

bool deletes_emit_one_each() {
    Fixture fixture;
    for (const char character : std::string_view("AB")) {
        Event event = text_event(static_cast<std::uint32_t>(character));
        fixture.runtime.handle_event(event);
    }
    Event home = key_event(key::kHome);
    if (!expect(fixture.runtime.handle_event(home), "Home consumed") ||
        !expect(fixture.runtime.change_events() == 2, "selection moves do not emit")) {
        return false;
    }
    Event remove_forward = key_event(key::kDelete);
    if (!expect(fixture.runtime.handle_event(remove_forward), "Delete consumed") ||
        !expect(fixture.runtime.change_events() == 3, "Delete emits one change") ||
        !expect(fixture.sink.value == "B", "Delete removed the first character") ||
        !expect(fixture.sink.selection_start == 0 && fixture.sink.selection_end == 0,
                "collapsed selection at the caret")) {
        return false;
    }
    Event end = key_event(key::kEnd);
    Event backspace = key_event(key::kBackspace);
    return expect(fixture.runtime.handle_event(end), "End consumed") &&
           expect(fixture.runtime.handle_event(backspace), "Backspace consumed") &&
           expect(fixture.runtime.change_events() == 4, "Backspace emits one change") &&
           expect(fixture.sink.value.empty(), "buffer emptied");
}

bool repeat_semantics() {
    Fixture fixture;
    for (const char character : std::string_view("ABC")) {
        Event event = text_event(static_cast<std::uint32_t>(character));
        fixture.runtime.handle_event(event);
    }
    const std::size_t before = fixture.runtime.change_events();
    // A repeat that does not mutate (arrow) is silent.
    Event arrow = key_event(key::kArrowLeft, Modifier::None, true);
    if (!expect(fixture.runtime.handle_event(arrow), "repeated arrow consumed") ||
        !expect(fixture.runtime.change_events() == before,
                "a non-mutating auto-repeat emits nothing")) {
        return false;
    }
    // A repeat that really deletes is a committed edit of its own: one event for that deletion.
    Event backspace = key_event(key::kBackspace, Modifier::None, true);
    return expect(fixture.runtime.handle_event(backspace), "repeated backspace consumed") &&
           expect(fixture.runtime.change_events() == before + 1,
                  "each committed deletion emits once, never once per repeat tick extra") &&
           expect(fixture.sink.value == "AC", "the deletion is reflected");
}

bool silent_cases() {
    // Read-only controller: text events are ignored and nothing is emitted.
    {
        Fixture fixture;
        fixture.controller.set_read_only(true);
        Event insert = text_event('x');
        if (!expect(!fixture.runtime.handle_event(insert), "read-only text ignored") ||
            !expect(fixture.runtime.change_events() == 0, "read-only emits nothing") ||
            !expect(!insert.handled, "ignored event is not marked handled")) {
            return false;
        }
    }
    // Unfocused node: the runtime declines even though the controller could edit.
    {
        Fixture fixture(true, false);
        Event insert = text_event('x');
        if (!expect(!fixture.runtime.handle_event(insert), "unfocused field ignored") ||
            !expect(fixture.runtime.change_events() == 0, "unfocused emits nothing")) {
            return false;
        }
    }
    // Detached runtime.
    {
        Fixture fixture;
        fixture.runtime.detach();
        Event insert = text_event('x');
        if (!expect(!fixture.runtime.handle_event(insert), "detached runtime ignored") ||
            !expect(fixture.runtime.change_events() == 0, "detached emits nothing")) {
            return false;
        }
    }
    // prevent_default suppresses the edit itself, so no change event either.
    {
        Fixture fixture;
        Event insert = text_event('x');
        insert.prevent_default = true;
        return expect(!fixture.runtime.handle_event(insert), "prevent_default ignored") &&
               expect(fixture.runtime.change_events() == 0, "suppressed edit emits nothing");
    }
}

bool route_log_equality_without_binding() {
    Fixture bound;
    Fixture unbound(false);
    for (Fixture* fixture : {&bound, &unbound}) {
        for (const char character : std::string_view("AB")) {
            Event event = text_event(static_cast<std::uint32_t>(character));
            fixture->runtime.handle_event(event);
        }
        Event backspace = key_event(key::kBackspace);
        fixture->runtime.handle_event(backspace);
    }
    if (!expect(bound.log_without(EventType::TextChange) ==
                    unbound.log_without(EventType::TextChange),
                "no-binding route log equals the bound route log") ||
        !expect(bound.runtime.change_events() == unbound.runtime.change_events(),
                "unbinding does not change the emission count") ||
        !expect(bound.runtime.last_change_handled(), "bound change is handled") ||
        !expect(!unbound.runtime.last_change_handled(), "unbound change is not marked handled")) {
        return false;
    }
    return true;
}

bool borrowed_full_value() {
    Fixture fixture;
    const std::string first(200, 'a');
    const std::string second(200, 'b');
    Event first_event = payload_event(EventType::EditText, first);
    Event second_event = payload_event(EventType::EditText, second);
    if (!expect(fixture.runtime.handle_event(first_event), "first commit consumed") ||
        !expect(fixture.runtime.handle_event(second_event), "second commit consumed") ||
        !expect(fixture.runtime.change_events() == 2, "two commits, two changes") ||
        !expect(fixture.sink.value == first + second,
                "payload value is the full text beyond the inline 256-byte copy") ||
        !expect(fixture.sink.selection_start == 400 && fixture.sink.selection_end == 400,
                "byte offsets cover the full text")) {
        return false;
    }
    return true;
}

bool forwarded_through_node_bindings() {
    // Generated components do not override Node::on_event: they register event bindings instead.
    // bind_text_input_events installs the six source-event forwarders for the runtime.
    Fixture fixture;
    if (!expect(tessera::ui::text::bind_text_input_events(*fixture.input, fixture.runtime),
                "source events bound to the runtime")) {
        return false;
    }
    Event insert = text_event('A');
    if (!expect(tessera::ui::dispatch_event_to(*fixture.input, insert),
                "Text dispatched through the scene path") ||
        !expect(fixture.runtime.change_events() == 1, "forwarded insertion emitted one change") ||
        !expect(fixture.sink.value == "A", "forwarded value")) {
        return false;
    }
    Event suppressed = text_event('B');
    suppressed.prevent_default = true;
    tessera::ui::dispatch_event_to(*fixture.input, suppressed);
    if (!expect(fixture.runtime.change_events() == 1,
                "prevent_default still suppresses forwarding")) {
        return false;
    }
    Event start = payload_event(EventType::CompositionStart, {});
    Event update = payload_event(EventType::CompositionUpdate, "にほ");
    Event commit = payload_event(EventType::EditText, "日");
    tessera::ui::dispatch_event_to(*fixture.input, start);
    tessera::ui::dispatch_event_to(*fixture.input, update);
    if (!expect(fixture.runtime.change_events() == 1, "forwarded preedit stays silent")) {
        return false;
    }
    tessera::ui::dispatch_event_to(*fixture.input, commit);
    return expect(fixture.runtime.change_events() == 2, "forwarded commit emitted one change") &&
           expect(fixture.sink.value == "A日", "forwarded commit value") &&
           expect(fixture.sink.selection_start == 4 && fixture.sink.selection_end == 4,
                  "forwarded selection byte offsets");
}

bool determinism() {
    const auto run = [](Fixture& fixture) {
        for (const char character : std::string_view("hello")) {
            Event event = text_event(static_cast<std::uint32_t>(character));
            fixture.runtime.handle_event(event);
        }
        Event left = key_event(key::kArrowLeft, Modifier::Shift);
        fixture.runtime.handle_event(left);
        Event backspace = key_event(key::kBackspace);
        fixture.runtime.handle_event(backspace);
    };
    Fixture first;
    Fixture second;
    run(first);
    run(second);
    return expect(first.sink.value == second.sink.value, "same final value") &&
           expect(first.sink.selection_start == second.sink.selection_start &&
                      first.sink.selection_end == second.sink.selection_end,
                  "same selection bytes") &&
           expect(first.runtime.change_events() == second.runtime.change_events(),
                  "same change count") &&
           expect(first.root.log == second.root.log, "same route log");
}

} // namespace

int main() {
    if (!insert_commit_once()) {
        return 1;
    }
    if (!one_change_per_edit()) {
        return 2;
    }
    if (!composition_commit_once()) {
        return 3;
    }
    if (!deletes_emit_one_each()) {
        return 4;
    }
    if (!repeat_semantics()) {
        return 5;
    }
    if (!silent_cases()) {
        return 6;
    }
    if (!route_log_equality_without_binding()) {
        return 7;
    }
    if (!borrowed_full_value()) {
        return 8;
    }
    if (!determinism()) {
        return 9;
    }
    if (!forwarded_through_node_bindings()) {
        return 10;
    }

    std::cout << "text_input_runtime: ok\n";
    return 0;
}
