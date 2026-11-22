// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/color.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// The `background-image` gradient value (issues #457 and #613).
//
// One gradient layer: `linear-gradient()` or `radial-gradient()`, each optionally prefixed with
// `repeating-`, with 2 to 8 colour stops. Anything else (conic, several layers, `url()`, a length
// stop position, a colour hint, an `at` position this grammar cannot resolve) is not a value of
// this grammar and `parse_gradient` returns nullopt, so the caller can report it instead of
// painting a guess.
//
// The repeating forms are a stop-list transform, not a second paint path: `expand_repeating_stops`
// turns a repeating ramp into an ordinary stop list that already spans the whole gradient
// line/radius, so every consumer downstream sees one kind of gradient.
namespace tessera::ui::style {

// The authored stop count of one gradient (CSS allows any number; this grammar caps it at 8).
inline constexpr std::size_t kMaxGradientStops = 8;
// The cap on an expanded repeating ramp. A ramp shorter than 1/16 of the line/radius is truncated
// here, which is the documented approximation: a browser would keep repeating into the last pixel.
inline constexpr std::size_t kMaxRepeatingStops = 32;

struct GradientStopValue {
    float offset = 0.0f; // 0..1 along the gradient line, already fixed up
    ColorRgba color;
};

// CSS convention: 0 points up, 90 right, 180 down (the default).
struct LinearGradientValue {
    float angle_degrees = 180.0f;
    std::array<GradientStopValue, kMaxGradientStops> stops{};
    std::size_t stop_count = 0;
};

// `radial-gradient()` shape. `circle` has one radius; `ellipse` (the default) has two.
enum class RadialShape : std::uint8_t { Circle, Ellipse };

// The size keywords of CSS Images 3 section 3.2.1, plus explicit radii. `Explicit` carries the two
// radii in `RadialGradientValue::radius_x`/`radius_y` (a circle uses `radius_x` only).
enum class RadialSize : std::uint8_t {
    ClosestSide,
    ClosestCorner,
    FarthestSide,
    FarthestCorner,
    Explicit
};

// The gradient's centre as a fraction of the node's border box, so paint resolves it against the
// box it actually paints (the parser has no box). CSS `at 25% 75%` is `{0.25, 0.75}`.
struct RadialPosition {
    float x = 0.5f;
    float y = 0.5f;
};

struct RadialGradientValue {
    RadialShape shape = RadialShape::Ellipse;
    RadialSize size = RadialSize::FarthestCorner;
    // Only meaningful for `RadialSize::Explicit`: the radii in px. A circle uses `radius_x`.
    float radius_x = 0.0f;
    float radius_y = 0.0f;
    RadialPosition position;
    std::array<GradientStopValue, kMaxGradientStops> stops{};
    std::size_t stop_count = 0;
};

// `conic-gradient()` (issue #617). The ramp sweeps clockwise from `from_angle_degrees` around
// `position`, which is a fraction of the node's border box like a radial gradient's centre. CSS
// measures the angle from 12 o'clock (0deg = up, 90deg = right), which is the same convention the
// linear form uses. A stop's offset is a fraction of the full turn.
struct ConicGradientValue {
    float from_angle_degrees = 0.0f;
    RadialPosition position;
    std::array<GradientStopValue, kMaxGradientStops> stops{};
    std::size_t stop_count = 0;
};

// The tagged union a `background-image` gradient resolves to. `repeating` is carried rather than
// baked into the stops so a caller can tell the two forms apart (diagnostics, tests, a future
// per-pixel path); the stops are the authored ramp either way, and `expand_repeating_stops` is what
// turns a repeating one into a paintable list.
//
// The three shapes share one slot (issue #731). A value is exactly one shape, so carrying all three
// side by side paid for 24 stops where a gradient can use 8: `GradientValue` was 560 bytes and
// `BackgroundLayerList` (8 layers) 4552, which is what `resolve_layers` returned by value once per
// painted node. The union is 200 bytes and the list 1672. `kind` says which member is live, and
// every reader already switches on it.
struct GradientValue {
    enum class Kind : std::uint8_t { Linear, Radial, Conic } kind = Kind::Linear;
    bool repeating = false;
    union {
        LinearGradientValue linear;
        RadialGradientValue radial;
        ConicGradientValue conic;
    };

    GradientValue()
        : linear{} {}
};

// Offsets follow CSS Images 3 section 3.4.3: a missing first/last position is 0/100 %, a missing
// middle one is spread evenly between its neighbours, and a position behind an earlier one is
// raised to it. A corner direction (`to top right`) resolves to the diagonal angle of a square box;
// CSS bends it with the box's aspect ratio, which this does not.
//
// `parse_gradient` accepts both shapes and both repeating forms. `parse_linear_gradient` is the
// linear-only entry point kept for callers that only ever want a linear ramp; it returns nullopt
// for a radial value. Both share one grammar implementation.
[[nodiscard]] std::optional<GradientValue> parse_gradient(std::string_view text);
[[nodiscard]] std::optional<LinearGradientValue> parse_linear_gradient(std::string_view text);

// True when `text` starts with a gradient function name this grammar knows (`linear-gradient(`,
// `radial-gradient(`, or either with the `repeating-` prefix), case-insensitively. The `background`
// shorthand uses it to tell a gradient layer from a colour without parsing twice.
[[nodiscard]] bool is_gradient_function(std::string_view text);

// Expands a repeating ramp so it covers the whole [0,1] gradient line/radius: the authored cycle is
// repeated forwards and backwards until it spans 0..1, capped at `kMaxRepeatingStops` entries.
// `stops` must have room for `kMaxRepeatingStops` entries and `count` is the authored stop count.
// Returns the expanded count; a ramp with no positive period is returned unchanged. A non-repeating
// gradient never calls this.
[[nodiscard]] std::size_t expand_repeating_stops(GradientStopValue* stops, std::size_t count);

} // namespace tessera::ui::style
