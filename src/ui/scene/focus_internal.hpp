// SPDX-License-Identifier: MIT

// Helpers the focus code shares (issue #1035). Not public API.

#pragma once

#include "tessera/ui/scene/node.hpp"

namespace tessera::ui {

// A roving-focus group skips items that report `aria-disabled="true"`: a disabled Radio is neither
// the group's Tab stop nor an arrow-key target.
[[nodiscard]] inline bool aria_disabled(const Node& node) {
    for (const AriaAttribute& attribute : node.aria_attributes()) {
        if (attribute.name == "aria-disabled") {
            return attribute.value == "true";
        }
    }
    return false;
}

} // namespace tessera::ui
