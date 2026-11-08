// SPDX-License-Identifier: MIT

#include "tessera/ui/input.hpp"

#include <iostream>
#include <stdexcept>

namespace {

using tessera::ui::has_modifier;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputQueue;
using tessera::ui::InputSampler;
using tessera::ui::Modifier;
using tessera::ui::PointerButton;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "input: " << message << "\n";
    }
    return condition;
}

InputEvent move_event(std::uint32_t pointer, float x, float y, float dx, float dy,
                      double timestamp = 0.0) {
    InputEvent event;
    event.type = InputEventType::PointerMove;
    event.pointer_id = pointer;
    event.x = x;
    event.y = y;
    event.dx = dx;
    event.dy = dy;
    event.timestamp_s = timestamp;
    return event;
}

} // namespace

int main() {
    InputQueue queue(3);
    if (!expect(queue.capacity() == 3, "requested capacity") ||
        !expect(queue.empty(), "new queue is empty")) {
        return 1;
    }

    InputEvent first = move_event(1, 1, 2, 0.5f, 0.25f, 1.0);
    InputEvent second = move_event(1, 3, 4, 0.5f, 0.75f, 2.0);
    InputEvent third = move_event(2, 5, 6, 0, 0, 3.0);
    if (!expect(queue.push(first), "push first") || !expect(queue.push(second), "push second") ||
        !expect(queue.push(third), "push third") || !expect(queue.size() == 3, "queue size") ||
        !expect(!queue.push(first), "full queue rejects") ||
        !expect(queue.overflowed(), "overflow flag")) {
        return 2;
    }

    InputEvent out;
    if (!expect(queue.pop(out), "pop first") || !expect(out.x == 1 && out.y == 2, "FIFO first") ||
        !expect(queue.pop(out), "pop second") || !expect(out.x == 3 && out.y == 4, "FIFO second")) {
        return 3;
    }
    queue.clear_overflow();
    if (!expect(!queue.overflowed(), "clear overflow") ||
        !expect(queue.push(first), "push after pop")) {
        return 4;
    }

    InputSampler sampler(8);
    InputQueue raw(16);
    InputEvent down;
    down.type = InputEventType::PointerDown;
    down.pointer_id = 1;
    down.x = 10;
    down.y = 10;
    down.button = PointerButton::Left;
    down.modifiers = Modifier::Shift | Modifier::Control;

    raw.push(move_event(1, 1, 1, 1, 0, 1.0));
    raw.push(move_event(1, 2, 2, 1, 1, 2.0));
    raw.push(move_event(2, 3, 3, 0, 0, 3.0));
    raw.push(move_event(1, 4, 4, 0, 0, 4.0));
    raw.push(down);
    raw.push(move_event(1, 5, 5, 2, 2, 5.0));
    sampler.sample(raw);

    const auto& events = sampler.events();
    if (!expect(events.size() == 5, "coalesce adjacent moves") ||
        !expect(events[0].type == InputEventType::PointerMove, "first is move") ||
        !expect(events[0].x == 2 && events[0].y == 2, "latest move position") ||
        !expect(events[0].dx == 2 && events[0].dy == 1, "accumulated move delta") ||
        !expect(events[0].timestamp_s == 2.0, "latest move timestamp") ||
        !expect(events[1].pointer_id == 2, "second pointer preserved") ||
        !expect(events[2].pointer_id == 1, "interleaved pointer preserved") ||
        !expect(events[3].type == InputEventType::PointerDown, "down preserved") ||
        !expect(events[4].type == InputEventType::PointerMove, "move after down preserved") ||
        !expect(events[4].dx == 2 && events[4].dy == 2, "move after down not merged")) {
        return 5;
    }

    if (!expect(has_modifier(down.modifiers, Modifier::Shift), "shift modifier") ||
        !expect(has_modifier(down.modifiers, Modifier::Control), "control modifier") ||
        !expect(!has_modifier(down.modifiers, Modifier::Alt), "alt not set")) {
        return 6;
    }

    sampler.sample(raw);
    if (!expect(sampler.events().empty(), "empty queue samples empty")) {
        return 7;
    }

    bool threw = false;
    try {
        InputQueue invalid(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "zero queue capacity rejected")) {
        return 8;
    }

    threw = false;
    try {
        InputSampler invalid(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "zero sampler capacity rejected")) {
        return 9;
    }

    std::cout << "input: ok\n";
    return 0;
}
