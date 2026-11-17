// SPDX-License-Identifier: MIT
//
// The per-window anti-aliasing policy (docs/design/display-aware-rendering.md sections 2 and 5).
// Display facts and the renderer configuration resolve to one plain value, once per window and per
// fact change, so no AA decision reads the OS or the config on the frame path. This header only
// decides; the shader, the glyph rasterizer and the clip stack adopt the fields in later slices.

#pragma once

#include "tessera/hal/display.hpp"

#include <cstdint>

namespace tessera::ui::scale {

// `Auto` is the policy default; `Grayscale` and `Subpixel` are the explicit user settings.
enum class TextAaSetting : std::uint8_t { Auto, Grayscale, Subpixel };

// `Grayscale` is the only mode every run can use. `LcdWhenEligible` means the *window* permits LCD
// text; each run still needs its own eligibility check (opaque, translate-only, nothing beneath),
// which belongs to the text slice, not to this window-level value.
enum class TextAaMode : std::uint8_t { Grayscale, LcdWhenEligible };

// `Font` is today's FreeType default (the font's own hints). `Light` is vertical-only autohint.
enum class TextHinting : std::uint8_t { Font, Light, None };

// `Off` is hard pixel-stable edges (golden mode or AA disabled). `FixedRadius` is the current
// 0.5-local-unit radius. `ScreenSpace` derives the width from fwidth() so scale and rotation keep
// about one device pixel.
enum class EdgeMode : std::uint8_t { Off, FixedRadius, ScreenSpace };

// The renderer configuration inputs (antialiasing.md section 3). Presets only, no per-node radius.
struct AaConfig {
    TextAaSetting text_aa = TextAaSetting::Auto;
    bool aa_enabled = true;
    // 1 or 4; any other value resolves to 1.
    std::uint8_t msaa_samples = 1;
    // The power policy's cheaper preset (ADR-019): forces grayscale text and no MSAA.
    bool reduced_quality = false;
    // Deterministic-test mode: ignores every other input and every display fact.
    bool golden_mode = false;
};

// The display facts the policy reads; copied from `WindowMetrics` by the caller.
struct AaFacts {
    // A ladder step (see render_scale.hpp); a non-ladder value is quantized, an invalid one is 1.0.
    float render_scale = 1.0f;
    hal::PixelGeometry pixel_geometry = hal::PixelGeometry::Unknown;
    // `WindowMetrics::transparent`: a translucent client area never gets LCD text.
    bool transparent_window = false;
    hal::Orientation orientation = hal::Orientation::Landscape;
};

struct AaPolicy {
    float render_scale = 1.0f;
    TextAaMode text_mode = TextAaMode::Grayscale;
    TextHinting hinting = TextHinting::Font;
    bool subpixel_position = false;
    EdgeMode edge_mode = EdgeMode::FixedRadius;
    bool clip_aa = false;
    std::uint8_t msaa_samples = 1;

    friend bool operator==(const AaPolicy&, const AaPolicy&) = default;
};

// `Auto` does not yet choose LCD: shipping LCD text is an open owner decision (display-aware-
// rendering.md section 8, item 1). Flipping this is the whole change once it is decided.
inline constexpr bool kAutoAllowsLcd = false;

// Render scale from which glyph origins carry a quarter-pixel horizontal fraction.
inline constexpr float kSubpixelPositionMinScale = 1.25f;
// Render scale from which hinting is switched off (density hides it).
inline constexpr float kNoHintingMinScale = 2.0f;

// The fixed policy golden mode pins: scale 1.0, grayscale, the font's own hints, whole-pixel glyph
// origins, hard edges, no clip AA, one sample.
[[nodiscard]] AaPolicy golden_aa_policy() noexcept;

// Pure and allocation-free. Total: invalid inputs fall back to the safest value (grayscale, 1.0).
[[nodiscard]] AaPolicy resolve_aa_policy(const AaFacts& facts, const AaConfig& config) noexcept;

} // namespace tessera::ui::scale
