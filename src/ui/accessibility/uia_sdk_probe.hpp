// SPDX-License-Identifier: MIT

#pragma once

#if !defined(_WIN32)
#error "uia_sdk_probe.hpp is Windows-only; guard the include with #if defined(_WIN32)"
#endif

namespace tessera::ui::a11y::win {

// Proves that the Windows toolchain can compile against `UIAutomationCore.h` and
// `UIAutomationClient.h` and link `uiautomationcore` + `ole32` from the static accessibility
// library. It also static_asserts every UI Automation id the platform-neutral mapping repeats, so
// a drifted constant fails the Windows build instead of a client call at runtime. Called by
// `a11y_uia_compile_check_tests`; it never creates a provider or touches a real client.
[[nodiscard]] bool uia_sdk_probe() noexcept;

} // namespace tessera::ui::a11y::win
