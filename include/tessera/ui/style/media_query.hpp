// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/detail/cache_line.hpp"
#include "tessera/hal/appearance.hpp"
#include "tessera/hal/window_backdrop.hpp"
#include "tessera/hal/window_cutout_fold.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style {

enum class ColorScheme : std::uint8_t {
    Light,
    Dark,
};

// `@media (hover: ...)`: whether the primary device can hover.
enum class MediaHover : std::uint8_t {
    None,
    Hover,
};

// `@media (pointer: ...)`: the primary device's accuracy.
enum class MediaPointer : std::uint8_t {
    None,
    Coarse,
    Fine,
};

// `@media (device-posture: ...)` (Device Posture API, section 6.1): `folded` for a device whose
// fold is half open, `continuous` for everything else, including a flat foldable and a device with
// no fold at all. Exactly one of the two is always true. The angle is deliberately not here: it is
// continuous, so putting it in the context would re-resolve the cascade per degree (ADR-074).
enum class MediaPosture : std::uint8_t {
    Continuous,
    Folded,
};

struct MediaContext {
    double viewport_width = 1280.0;
    double viewport_height = 720.0;
    double resolution_dpi = 96.0;
    ColorScheme color_scheme = ColorScheme::Light;
    bool prefers_reduced_motion = false;
    // The material the platform resolved for the window (glass-v1.md section 6). `None` is the
    // default because a headless/null window offers no material, so a stylesheet's opaque fallback
    // inside `@media (window-backdrop: none)` applies where the engine cannot show one.
    hal::BackdropMaterial window_backdrop = hal::BackdropMaterial::NoMaterial;
    // The primary pointing device's capabilities (issue #691). A desktop window with a mouse
    // answers `hover: hover` and `pointer: fine`; a touch-only device answers `hover: none` and
    // `pointer: coarse`. The Studio's device preset supplies these for the simulation, so a
    // stylesheet can be previewed as a phone without a phone.
    MediaHover hover = MediaHover::Hover;
    MediaPointer pointer = MediaPointer::Fine;
    // The `any-*` forms: true when ANY available device has the capability, which is what a hybrid
    // laptop with a touchscreen answers. Defaults match the primary device.
    MediaHover any_hover = MediaHover::Hover;
    MediaPointer any_pointer = MediaPointer::Fine;
    // The fold, as the discrete answers Media Queries Level 5 defines (ADR-074). A device with no
    // separating fold is one segment on each axis. `with_fold` is the one mapping from `FoldInfo`.
    MediaPosture device_posture = MediaPosture::Continuous;
    std::uint8_t horizontal_viewport_segments = 1;
    std::uint8_t vertical_viewport_segments = 1;
};

enum class MediaFeature : std::uint8_t {
    Width,
    Height,
    Resolution,
    Orientation,
    PrefersColorScheme,
    PrefersReducedMotion,
    // `@media (window-backdrop: none | mica | acrylic | blur)` on the platform's resolved material
    // (glass-v1.md section 6). It is not a request: `auto` never matches.
    WindowBackdrop,
    // `@media (aspect-ratio: 16/9)` and its `min-`/`max-` forms (issue #691). Compared as a ratio
    // of two integers, never as a float, so `16/9` is exact.
    AspectRatio,
    Hover,
    AnyHover,
    Pointer,
    AnyPointer,
    // `@media (device-posture: continuous | folded)`. Not a range feature: no `min-`/`max-`.
    DevicePosture,
    // `@media (horizontal-viewport-segments: 2)` and the vertical twin: the number of logical
    // regions a separating fold divides the viewport into. Integer range features.
    HorizontalViewportSegments,
    VerticalViewportSegments,
};

enum class MediaComparison : std::uint8_t {
    Equal,
    Min,
    Max,
    // The range form (issue #691): `(width >= 600px)` and friends. `Min`/`Max` are the legacy
    // `min-`/`max-` prefixes, which are inclusive; these are the explicit operators, which are not
    // always.
    Greater,
    GreaterEqual,
    Less,
    LessEqual,
};

struct MediaCondition {
    MediaFeature feature = MediaFeature::Width;
    MediaComparison comparison = MediaComparison::Equal;
    std::string value;
    // The upper bound of a two-sided range (`600px <= width <= 900px`), which is the one form that
    // needs two values. Empty for every other form.
    std::string upper_value;
    MediaComparison upper_comparison = MediaComparison::Equal;
};

// `alignas(64)` (issue #732): `bool` + `std::string` (32) + `std::vector` (24) is exactly 64, so
// with the natural 8-byte alignment seven of eight queries straddle a line. The alignment changes
// no offset.
//
// MSVC's C4324 describes that intent, so it is suppressed here the same narrow way as `Rule` in
// `parser.hpp` and the groups in `detail/cache_line.hpp` (issue #898).
TESSERA_CACHE_LINE_BEGIN
struct alignas(64) MediaQuery {
    bool negated = false;
    std::string type;
    std::vector<MediaCondition> conditions;
};
TESSERA_CACHE_LINE_END

struct MediaQueryList {
    std::vector<MediaQuery> queries;
};

struct MediaQueryParseResult {
    MediaQueryList list;
    bool ok = true;
    std::size_t error_offset = 0;
};

MediaQueryParseResult parse_media_query_list(std::string_view text);
bool evaluate(const MediaQuery& query, const MediaContext& context);
bool evaluate(const MediaQueryList& queries, const MediaContext& context);

// Folds the window's fold into a media context (ADR-074): `device-posture` is `folded` when the
// fold is half open, and a fold that `separating` splits the viewport into two segments along the
// axis the hinge crosses (a vertical hinge gives two horizontal segments). The hinge angle is not
// read: it never reaches the media context. Everything else in `context` is left as the caller set
// it.
[[nodiscard]] MediaContext with_fold(MediaContext context, const hal::FoldInfo& fold) noexcept;

// Folds the OS appearance snapshot into a media context: `prefers-color-scheme` follows the
// resolved color scheme and `prefers-reduced-motion` the reduced-motion flag (issue #187). It is
// the one place the HAL's normalized preference becomes CSS, so the mapping is tested directly and
// no host has to re-derive it. Everything else in `context` is left as the caller set it (viewport,
// resolution, backdrop), because those come from the window metrics, not the settings.
[[nodiscard]] MediaContext with_appearance(MediaContext context,
                                           const hal::AppearancePreference& appearance) noexcept;

} // namespace tessera::ui::style
