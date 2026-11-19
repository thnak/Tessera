// SPDX-License-Identifier: MIT

#include "volume_dispatch.hpp"

namespace tessera::ui {

InputDispatchResult dispatch_volume_key(Node* target, const InputEvent& input) {
    InputDispatchResult result;
    result.type = EventType::VolumeKey;
    if (target == nullptr) {
        return result;
    }
    Event event;
    event.type = EventType::VolumeKey;
    event.timestamp_s = input.timestamp_s;
    event.volume_direction = input.volume_direction;
    result.target = target;
    result.target_id = target->id();
    result.dispatched = dispatch_event_to(*target, event);
    return result;
}

} // namespace tessera::ui
