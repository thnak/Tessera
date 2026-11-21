// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/window.hpp"

#include <cstdint>

// Pure decode of the Android window-metrics signals into `hal::WindowMetrics` (issue #189,
// docs/design/mobile-integration.md sections 3.5 and 3.6).
//
// This header deliberately does not include any Android or JNI header: the values below are the
// plain numbers the platform reports, so the rules are unit-testable on every lane. The JNI bridge
// (android_metrics.cpp) reads `WindowInsets`, `DisplayCutout` and `FoldingFeature` and calls these.
//
// The platform reports insets in **physical pixels**; `WindowMetrics` carries logical pixels, so
// every rule divides by the display scale. That conversion is the whole reason this is a separate,
// testable unit: a wrong scale turns a 24 px status bar into a 24 dp one, which is a layout bug
// that only shows on a device with a non-1.0 density.

namespace tessera::hal::android {

// The inset types Android reports (`WindowInsets.Type`). The values are the platform's bit
// positions, restated so this header needs no Android include.
enum class InsetType : std::uint32_t {
    StatusBars = 1U << 0,
    NavigationBars = 1U << 1,
    CaptionBar = 1U << 2,
    SystemBars = 1U << 3,
    DisplayCutout = 1U << 4,
    Ime = 1U << 5,
    SystemGestures = 1U << 6,
    MandatorySystemGestures = 1U << 7,
    TappableElement = 1U << 8,
};

// One side's inset in physical pixels, as the platform reports it.
struct PhysicalInsets {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
};

// A cutout rectangle in physical pixels, window coordinates.
struct PhysicalRect {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
};

// The fold state Android's `FoldingFeature` reports (`FoldingFeature.State`).
enum class FoldPosture : std::uint8_t {
    None,
    Flat,
    HalfOpened,
};

// The fold orientation (`FoldingFeature.Orientation`).
enum class FoldAxis : std::uint8_t {
    None,
    Horizontal,
    Vertical,
};

// Converts one side's physical inset to logical pixels. A negative value is the platform's "no
// inset" and becomes zero; the result is rounded, not truncated, so a 2.75x device does not lose a
// pixel per side.
[[nodiscard]] float logical_inset(std::int32_t physical, float scale) noexcept;

// The system-bar insets (status + navigation) as `WindowMetrics::insets` wants them.
[[nodiscard]] WindowInsets system_bar_insets(const PhysicalInsets& physical, float scale) noexcept;

// The IME insets as `WindowMetrics::ime_insets` wants them.
[[nodiscard]] WindowInsets ime_insets(const PhysicalInsets& physical, float scale) noexcept;

// The system-gesture insets as `WindowMetrics::gesture_insets` wants them.
[[nodiscard]] WindowInsets gesture_insets(const PhysicalInsets& physical, float scale) noexcept;

// The cutout list as `WindowMetrics::cutouts` wants it. `rects` is the platform's
// `DisplayCutout.getBoundingRects()`; at most `kMaxCutouts` are kept, and a rectangle with no area
// is dropped rather than reported as a zero-size cutout.
[[nodiscard]] CutoutInfo cutout_info(std::span<const PhysicalRect> rects, float scale) noexcept;

// The fold state as `WindowMetrics::fold` wants it. `bounds` is the hinge rectangle in physical
// pixels; it is ignored unless the posture is `HalfOpened`, because a flat fold has no occluding
// hinge.
[[nodiscard]] FoldInfo fold_info(FoldPosture posture, FoldAxis axis, const PhysicalRect& bounds,
                                 float scale) noexcept;

// The window orientation from the logical extent. Android reports a rotation, but the engine
// derives the orientation from the extent everywhere else, so a device that reports neither still
// gets the same answer as the desktop lanes.
[[nodiscard]] Orientation orientation_from_extent(std::uint32_t logical_width,
                                                  std::uint32_t logical_height) noexcept;

} // namespace tessera::hal::android
