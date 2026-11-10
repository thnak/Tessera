// SPDX-License-Identifier: MIT
//
// Windows-only WM_GETOBJECT answer for the HAL window. The whole point of this translation unit is
// the include order: `basetyps.h` (reached through windows.h) defines the `interface` macro the
// generated UI Automation headers need, and it must come first.
//
// clang-format off
#include <windows.h>
#include <UIAutomationClient.h>
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
// clang-format on

#include "window_accessibility_win.hpp"

namespace tessera::hal::detail {

std::optional<std::intptr_t> answer_accessibility_get_object(void* window, std::uintptr_t wparam,
                                                             std::intptr_t lparam,
                                                             const AccessibilityRoot& root) {
    if (root.answer == nullptr) {
        return std::nullopt;
    }
    void* raw_provider = nullptr;
    // The callback only reads a pointer another thread published; nothing here waits for the
    // accessibility bridge, so a message pump is never stalled by a starting provider.
    if (!root.answer(root.context, lparam, raw_provider) || raw_provider == nullptr) {
        return std::nullopt;
    }
    return static_cast<std::intptr_t>(UiaReturnRawElementProvider(
        static_cast<HWND>(window), static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam),
        static_cast<::IRawElementProviderSimple*>(raw_provider)));
}

} // namespace tessera::hal::detail
