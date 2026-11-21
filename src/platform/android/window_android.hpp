// SPDX-License-Identifier: MIT
//
// Android window backend factory (issue #189). The implementation lives in window_android.cpp;
// this header lets a host call it without including <android/native_window.h>.

#pragma once

#include "tessera/hal/window.hpp"

#include <memory>

struct ANativeWindow;

namespace tessera::hal {

// Adopts `window` (the ANativeWindow* the host owns) and wraps it in the HAL contract. Returns
// nullptr when `window` is null, which is the documented "no surface yet" answer: Android creates
// and destroys surfaces as the app moves between foreground and background, so a host may call
// this before it has one.
//
// `dpi_scale` is the display density the platform reports (1.0 = mdpi); a non-positive value is
// treated as 1.0. The backend takes its own reference on `window`, so the host may release its own
// at any point after this call.
std::unique_ptr<Window> create_android_window(ANativeWindow* window, const WindowDesc& desc,
                                              float dpi_scale);

} // namespace tessera::hal
