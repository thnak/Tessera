// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"
#include "tessera/ui/scene/event.hpp"

namespace tessera::ui {

// IME/text plumbing (the text editor itself is a later slice). Composition and committed edit
// events are routed to the focused node only; `prevent_default` is advisory because there is no
// framework text insertion yet.
//
// Maps CompositionStart/Update/End/EditText to their scene `EventType`; returns false for any
// other input type.
[[nodiscard]] bool composition_event_type(InputEventType input, EventType& output);

// Builds the scene event with the inline UTF-8 payload and the UTF-16 code-unit selection range
// copied from the normalized event.
[[nodiscard]] Event make_composition_event(EventType type, const InputEvent& input);

} // namespace tessera::ui
