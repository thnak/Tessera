// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/node_fwd.hpp"

#include <memory>
#include <vector>

namespace tessera::ui {

// Visits `children` in stacking order without allocating: ascending z-index for painting (later
// children over earlier ones at equal z-index), or descending z-index with reversed sibling order
// for hit testing (`descending`). `fn(Node&)` returns true to stop; the result is true when it did.
// No child with a z-index (the common case) is a plain forward/backward walk, decided from each
// child's `has_z_index()` flag so the z values themselves are never read. A sibling set that all
// share one non-zero z-index takes the general path below, which orders it identically.
template<typename Fn>
bool for_each_by_z(const std::vector<std::unique_ptr<Node>>& children, bool descending, Fn&& fn) {
    bool uniform = true;
    for (const auto& child : children) {
        if (child->has_z_index()) {
            uniform = false;
            break;
        }
    }
    if (uniform) {
        if (descending) {
            for (auto it = children.rbegin(); it != children.rend(); ++it) {
                if (fn(**it)) {
                    return true;
                }
            }
        } else {
            for (const auto& child : children) {
                if (fn(*child)) {
                    return true;
                }
            }
        }
        return false;
    }

    bool have_previous = false;
    int previous = 0;
    for (;;) {
        bool found = false;
        int level = 0;
        for (const auto& child : children) {
            const int z = child->z_index();
            if (have_previous && (descending ? z >= previous : z <= previous)) {
                continue;
            }
            if (!found || (descending ? z > level : z < level)) {
                level = z;
                found = true;
            }
        }
        if (!found) {
            return false;
        }
        if (descending) {
            for (auto it = children.rbegin(); it != children.rend(); ++it) {
                if ((*it)->z_index() == level && fn(**it)) {
                    return true;
                }
            }
        } else {
            for (const auto& child : children) {
                if (child->z_index() == level && fn(*child)) {
                    return true;
                }
            }
        }
        previous = level;
        have_previous = true;
    }
}

} // namespace tessera::ui
