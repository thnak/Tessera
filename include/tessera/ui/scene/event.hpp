// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"

#include <cstdint>

namespace tessera::ui {

enum class EventType { PointerDown, PointerUp, PointerMove, KeyDown, KeyUp, Focus, Blur };

enum class EventPhase { Capture, Target, Bubble };

struct Event {
    EventType type = EventType::PointerDown;
    float x = 0;
    float y = 0;
    EventPhase phase = EventPhase::Target;
    bool handled = false;
    bool stopped = false;
    std::uint32_t key = 0;
    Modifier modifiers = Modifier::None;
    std::uint32_t text_codepoint = 0;
    bool repeat = false;

    void stop_propagation() {
        stopped = true;
    }
};

} // namespace tessera::ui
