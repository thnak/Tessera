// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"

namespace tessera::ui {

// A hardware volume key (docs/specs/studio-device-profiles-v1.md section 4): dispatches
// `EventType::VolumeKey` to `target` (the focused node) through the normal capture/target/bubble
// path. There is no default action. A null target (no focus) drops the key.
InputDispatchResult dispatch_volume_key(Node* target, const InputEvent& input);

} // namespace tessera::ui
