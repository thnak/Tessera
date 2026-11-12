// SPDX-License-Identifier: MIT
//
// The display-refresh value seam (docs/specs/runtime-config-v1.md sections 5.2 and 6.2;
// docs/design/performance.md section 6.1).
//
// A platform reports a refresh rate as a raw number from an OS API. Everything that decides what
// that number *means* - whether it is usable, what "unknown" is, and which value reaches
// WindowOptions::refresh_hz - lives in these pure functions, so the rules are testable without a
// display, a window or an OS call. The platform code stays thin: read the OS value, hand it here.

#pragma once

#include <cstdint>

namespace tessera::hal {

// The documented "unknown" value. 0 is not a rate: it means the platform could not determine the
// display refresh, so no divisor exists and the present cap keeps its configured value.
inline constexpr double kUnknownRefreshHz = 0.0;

// Sane bounds of a display refresh. Outside them a report is a sentinel or a broken value, never a
// mode a display runs at, so it maps to unknown instead of guessing.
inline constexpr double kMinDisplayRefreshHz = 1.0;
inline constexpr double kMaxDisplayRefreshHz = 1000.0;

// Validates an OS-reported refresh. Returns the value when it is finite and within
// [kMinDisplayRefreshHz, kMaxDisplayRefreshHz], otherwise kUnknownRefreshHz. Zero, negative, NaN,
// infinities and out-of-range values are all unknown; the function never throws.
[[nodiscard]] double sanitize_display_refresh_hz(double reported_hz) noexcept;

// The value that feeds WindowOptions::refresh_hz. An explicit caller value that is finite and
// positive always wins (the window host rejects an invalid one before this runs); otherwise the
// sanitized platform report is used, so "unknown" stays "unknown" (0).
[[nodiscard]] double resolve_display_refresh_hz(double explicit_hz, double reported_hz) noexcept;

// The X11/RandR mode calculation: the pixel clock divided by the total pixels of one frame.
// Returns kUnknownRefreshHz when the mode has no usable timing (a zero dot clock, a zero total, or
// a rate outside the sane bounds) - Xvfb's synthetic modes report exactly that.
[[nodiscard]] double refresh_hz_from_display_mode(std::uint64_t dot_clock_hz, std::uint32_t htotal,
                                                  std::uint32_t vtotal) noexcept;

} // namespace tessera::hal
