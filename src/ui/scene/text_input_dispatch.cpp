// SPDX-License-Identifier: MIT

#include "text_input_dispatch.hpp"

#include <algorithm>

namespace tessera::ui {

bool composition_event_type(InputEventType input, EventType& output) {
    switch (input) {
    case InputEventType::CompositionStart:
        output = EventType::CompositionStart;
        return true;
    case InputEventType::CompositionUpdate:
        output = EventType::CompositionUpdate;
        return true;
    case InputEventType::CompositionEnd:
        output = EventType::CompositionEnd;
        return true;
    case InputEventType::EditText:
        output = EventType::EditText;
        return true;
    default:
        return false;
    }
}

Event make_composition_event(EventType type, const InputEvent& input) {
    Event event;
    event.type = type;
    event.x = input.x;
    event.y = input.y;
    event.modifiers = input.modifiers;
    event.timestamp_s = input.timestamp_s;
    const std::size_t size = std::min<std::size_t>(input.text_size, tessera::ui::kMaxEditTextBytes);
    std::copy_n(input.text.begin(), size, event.text.begin());
    event.text_size = static_cast<std::uint32_t>(size);
    event.text_truncated = input.text_truncated;
    event.selection_start = input.selection_start;
    event.selection_end = input.selection_end;
    return event;
}

} // namespace tessera::ui
