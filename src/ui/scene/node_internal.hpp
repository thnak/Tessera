// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

// File-local helpers shared by the `ui::Node` responsibility TUs (node.cpp, node_geometry.cpp,
// node_scroll.cpp, node_interaction.cpp). They live in an internal header rather than in the public
// node.hpp because they are implementation details of the split, not part of the scene API
// (docs/architecture/god-type-audit-v1.md section 3.1).
namespace tessera::ui::detail {

// One scrollport inset: negative, NaN, and infinite values sanitize to 0 and the inset never takes
// more than the box extent left for it.
inline float clamp_inset(float value, float extent) {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return std::clamp(value, 0.0f, std::max(0.0f, extent));
}

} // namespace tessera::ui::detail
