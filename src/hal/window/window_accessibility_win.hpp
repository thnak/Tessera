// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/window.hpp"

#include <cstdint>
#include <optional>

namespace tessera::hal::detail {

// Answers one WM_GETOBJECT on Windows. Returns the LRESULT to return from the window procedure, or
// std::nullopt when the window must call DefWindowProc: no hook installed, the root provider is not
// ready (starting, failed, stopped), or the object id is not the UIA root id. The UIA call happens
// in this translation unit only, so the window procedure never includes the SDK headers.
[[nodiscard]] std::optional<std::intptr_t>
answer_accessibility_get_object(void* window, std::uintptr_t wparam, std::intptr_t lparam,
                                const AccessibilityRoot& root);

} // namespace tessera::hal::detail
