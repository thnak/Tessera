// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/input_dispatch.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::ui::Event;
using tessera::ui::EventPhase;
using tessera::ui::EventType;
using tessera::ui::InputDispatchResult;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::kMaxEditTextBytes;
using tessera::ui::Node;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "input_text: " << message << "\n";
    }
    return condition;
}

struct Recorder : Node {
    using Node::Node;

    void on_event(Event& event) override {
        events.push_back(event);
    }

    std::vector<Event> events;
};

InputEvent text_event(InputEventType type, std::string_view utf8, double timestamp_s = 0.0) {
    InputEvent event;
    event.type = type;
    event.timestamp_s = timestamp_s;
    const std::size_t size = std::min(utf8.size(), kMaxEditTextBytes);
    std::copy_n(utf8.begin(), size, event.text.begin());
    event.text_size = static_cast<std::uint32_t>(size);
    event.text_truncated = utf8.size() > kMaxEditTextBytes;
    return event;
}

InputEvent pointer(InputEventType type, float x, float y) {
    InputEvent event;
    event.type = type;
    event.x = x;
    event.y = y;
    return event;
}

bool carries(const Event& event, std::string_view expected) {
    return event.text_size == expected.size() &&
           std::equal(expected.begin(), expected.end(), event.text.begin());
}

} // namespace

int main() {
    Recorder root(1);
    root.set_bounds({0, 0, 200, 200});
    auto child_ptr = std::make_unique<Recorder>(2);
    child_ptr->set_bounds({10, 10, 50, 50});
    child_ptr->set_focusable(true);
    Recorder& child = static_cast<Recorder&>(root.add_child(std::move(child_ptr)));
    auto other_ptr = std::make_unique<Recorder>(3);
    other_ptr->set_bounds({100, 10, 50, 50});
    Recorder& other = static_cast<Recorder&>(root.add_child(std::move(other_ptr)));

    InputRouter router;

    // Without focus every composition/edit event is dropped (nothing is delivered).
    const InputEventType composition_types[] = {
        InputEventType::CompositionStart, InputEventType::CompositionUpdate,
        InputEventType::CompositionEnd, InputEventType::EditText};
    for (InputEventType type : composition_types) {
        const InputEvent input = text_event(type, "ab");
        const InputDispatchResult result = router.dispatch(root, input);
        if (!expect(!result.dispatched, "composition without focus is dropped") ||
            !expect(result.target == nullptr, "composition without focus has no target")) {
            return 1;
        }
    }

    router.focus_manager().request_focus(&child);
    child.events.clear();
    root.events.clear();

    const InputDispatchResult start =
        router.dispatch(root, text_event(InputEventType::CompositionStart, ""));
    if (!expect(start.dispatched, "composition start dispatched") ||
        !expect(start.type == EventType::CompositionStart, "composition start type") ||
        !expect(start.target == &child, "composition start target is the focus") ||
        !expect(child.events.size() == 1, "focused node received the start") ||
        !expect(child.events[0].type == EventType::CompositionStart, "start event mirror")) {
        return 2;
    }

    child.events.clear();
    InputEvent update = text_event(InputEventType::CompositionUpdate, "ab", 1.5);
    update.selection_start = 1;
    update.selection_end = 2;
    update.modifiers = tessera::ui::Modifier::Shift;
    const InputDispatchResult updated = router.dispatch(root, update);
    if (!expect(updated.dispatched && updated.target == &child, "composition update routed") ||
        !expect(child.events.size() == 1 && child.events[0].type == EventType::CompositionUpdate,
                "update event mirror") ||
        !expect(carries(child.events[0], "ab"), "update payload copied") ||
        !expect(child.events[0].text_size == 2, "update payload size") ||
        !expect(child.events[0].selection_start == 1 && child.events[0].selection_end == 2,
                "update UTF-16 selection offsets") ||
        !expect(child.events[0].timestamp_s == 1.5, "update timestamp") ||
        !expect(tessera::ui::has_modifier(child.events[0].modifiers, tessera::ui::Modifier::Shift),
                "update modifiers") ||
        !expect(other.events.empty(), "non-focused node receives nothing")) {
        return 3;
    }

    child.events.clear();
    root.events.clear();
    InputEvent committed = text_event(InputEventType::EditText, "\xF0\x9F\x98\x80");
    committed.text_truncated = true;
    const InputDispatchResult edited = router.dispatch(root, committed);
    if (!expect(edited.dispatched && edited.target == &child, "edit text routed") ||
        !expect(child.events.size() == 1 && child.events[0].type == EventType::EditText,
                "edit text mirror") ||
        !expect(child.events[0].text_size == 4, "edit text UTF-8 size") ||
        !expect(static_cast<unsigned char>(child.events[0].text[0]) == 0xF0,
                "edit text UTF-8 bytes") ||
        !expect(child.events[0].text_truncated, "truncation flag mirrored")) {
        return 4;
    }

    // The normal capture/target/bubble path runs for composition events too.
    if (!expect(root.events.size() == 2, "root observes capture and bubble") ||
        !expect(root.events[0].phase == EventPhase::Capture, "composition capture phase") ||
        !expect(root.events[1].phase == EventPhase::Bubble, "composition bubble phase")) {
        return 5;
    }

    child.events.clear();
    const InputDispatchResult ended =
        router.dispatch(root, text_event(InputEventType::CompositionEnd, ""));
    if (!expect(ended.dispatched && child.events.size() == 1 &&
                    child.events[0].type == EventType::CompositionEnd,
                "composition end routed")) {
        return 6;
    }

    // prevent_default is advisory for text events (no framework insertion): the dispatcher has no
    // default action to suppress and reports nothing as prevented.
    router.focus_manager().clear_focus();
    const InputDispatchResult unfocused = router.dispatch(root, committed);
    if (!expect(!unfocused.dispatched, "edit text without focus dropped")) {
        return 7;
    }

    // Pointer and key dispatch are unchanged alongside the text plumbing.
    const auto down = router.dispatch(root, pointer(InputEventType::PointerDown, 20, 20));
    if (!expect(down.dispatched && down.target == &child, "pointer down still dispatches") ||
        !expect(router.focus_manager().focused() == &child, "pointer down still focuses")) {
        return 8;
    }
    InputEvent key;
    key.type = InputEventType::KeyDown;
    key.key = 'A';
    const InputDispatchResult keyed = router.dispatch(root, key);
    return expect(keyed.dispatched && keyed.target == &child, "key routing unchanged") ? 0 : 9;
}
