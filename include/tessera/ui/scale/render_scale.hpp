// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace tessera::ui::scale {

// Device scale 1.0 is the 96 dpi reference that CSS/dp units resolve against.
inline constexpr std::uint32_t kReferenceDpi = 96;

// The decided rasterization ladder (design/window-resize.md §2.3): a window's device scale is
// snapped to one of these four values before it sizes render targets or keys glyph atlases, so a
// display that reports 1.0833 or 1.1667 does not mint its own rasterization. The ladder is the
// decided value set, not a uniform lattice: the step is 0.25 in 1.0..1.5 and 0.5 from 1.5 to 2.0
// (there is no 1.75 step).
inline constexpr std::array<float, 4> kRenderScaleLadder{1.0f, 1.25f, 1.5f, 2.0f};

inline constexpr float kMinRenderScale = kRenderScaleLadder.front();
inline constexpr float kMaxRenderScale = kRenderScaleLadder.back();
inline constexpr std::size_t kRenderScaleStepCount = kRenderScaleLadder.size();

enum class RenderScaleError : std::uint8_t {
    // WindowMetrics::dpi_scale was not finite or not positive: no ladder step applies to it.
    InvalidDeviceScale,
    // A dpi of 0 does not describe a display.
    InvalidDpi,
};

// Rounds `device_scale` onto kRenderScaleLadder: nearest step, ties rounded up (toward the larger
// scale), then clamped to [kMinRenderScale, kMaxRenderScale].
//
// Total by design: a non-finite or non-positive scale quantizes to kMinRenderScale so the paint
// path always has a usable step. Callers that must reject bad metrics use `quantize_device_scale`
// instead of this function.
[[nodiscard]] float quantize_render_scale(float device_scale) noexcept;

// Same rounding as `quantize_render_scale`, but an invalid device scale is reported instead of
// clamped, so a bad WindowMetrics frame cannot silently downgrade the rasterization.
[[nodiscard]] std::expected<float, RenderScaleError>
quantize_device_scale(float device_scale) noexcept;

// device_scale = dpi / kReferenceDpi. DPI 0 is rejected.
[[nodiscard]] std::expected<float, RenderScaleError>
device_scale_for_dpi(std::uint32_t dpi) noexcept;

// dpi / kReferenceDpi rounded onto the ladder; the DPI table the tests pin.
[[nodiscard]] std::expected<float, RenderScaleError>
render_scale_for_dpi(std::uint32_t dpi) noexcept;

// Position of an exact ladder value in kRenderScaleLadder, or -1 when `render_scale` is not a
// ladder value (for example a caller-supplied 1.75).
[[nodiscard]] std::int32_t render_scale_index(float render_scale) noexcept;

} // namespace tessera::ui::scale
