// SPDX-License-Identifier: MIT

#pragma once

namespace tessera::ui {

enum class EventType { PointerDown, PointerUp, PointerMove };

enum class EventPhase { Capture, Target, Bubble };

struct Event {
    EventType type = EventType::PointerDown;
    float x = 0;
    float y = 0;
    EventPhase phase = EventPhase::Target;
    bool handled = false;
    bool stopped = false;

    void stop_propagation() {
        stopped = true;
    }
};

} // namespace tessera::ui
