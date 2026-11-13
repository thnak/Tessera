// SPDX-License-Identifier: MIT
//
// Win32 OS backdrop integration for the window (glass-v1.md section 6). Private to
// src/platform/windows: the engine sees only hal::BackdropMaterial through WindowMetrics.
//
// Windows 11 22H2+ exposes DWMWA_SYSTEMBACKDROP_TYPE (Mica and Acrylic); Windows 10 has only the
// undocumented SetWindowCompositionAttribute accent policy, which is off unless the project opts in
// with `win10Acrylic: true`. The capability query and the resolution are split so the pure table
// (tessera/hal/window_backdrop.hpp) stays testable with a fake HAL.

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "tessera/hal/window_backdrop.hpp"

#include <windows.h>

namespace tessera::hal::detail {

// Windows 11 22H2 (build 22621) is the first release with the documented
// DWMWA_SYSTEMBACKDROP_TYPE attribute; a Windows 11 21H2 build is treated as the Windows 10 row
// (no documented material, `auto` resolves to none), which is the conservative reading of the
// "Windows 11 (22H2+)" row of glass-v1.md section 6.
[[nodiscard]] bool window_backdrop_is_windows_11() noexcept;

// The raw facts of this Windows installation. `win10_acrylic` is the project's opt-in; it is
// ignored unless the platform is Windows 10, because Windows 11 has the documented attribute.
[[nodiscard]] BackdropCapabilities query_backdrop_capabilities(bool win10_acrylic) noexcept;

// Applies the resolved material to `window`: the DWM system backdrop type on Windows 11, the
// accent policy on Windows 10, and the frame extension that lets a premultiplied-alpha swapchain
// reach the compositor. Returns the resolution the window reports; a failed OS call is the opaque
// fallback with `fallback` set, never an exception or a retry.
[[nodiscard]] BackdropResolution apply_window_backdrop(HWND window, WindowBackdrop request,
                                                       bool transparent,
                                                       bool win10_acrylic) noexcept;

} // namespace tessera::hal::detail
