// SPDX-License-Identifier: MIT

#include "tessera/hal/window.hpp"

namespace tessera::hal {

// Platforms without a native window backend keep the seam linkable so shared code can query it.
std::unique_ptr<Window> create_window(const WindowDesc&) {
    return nullptr;
}

} // namespace tessera::hal
