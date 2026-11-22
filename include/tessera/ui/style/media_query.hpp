// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/appearance.hpp"
#include "tessera/hal/window_backdrop.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style {

enum class ColorScheme : std::uint8_t {
    Light,
    Dark,
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
};

enum class MediaComparison : std::uint8_t {
    Equal,
    Min,
    Max,
};

struct MediaCondition {
    MediaFeature feature = MediaFeature::Width;
    MediaComparison comparison = MediaComparison::Equal;
    std::string value;
};

// `alignas(64)` (issue #732): `bool` + `std::string` (32) + `std::vector` (24) is exactly 64, so
// with the natural 8-byte alignment seven of eight queries straddle a line. The alignment changes
// no offset.
struct alignas(64) MediaQuery {
    bool negated = false;
    std::string type;
    std::vector<MediaCondition> conditions;
};

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

// Folds the OS appearance snapshot into a media context: `prefers-color-scheme` follows the
// resolved color scheme and `prefers-reduced-motion` the reduced-motion flag (issue #187). It is
// the one place the HAL's normalized preference becomes CSS, so the mapping is tested directly and
// no host has to re-derive it. Everything else in `context` is left as the caller set it (viewport,
// resolution, backdrop), because those come from the window metrics, not the settings.
[[nodiscard]] MediaContext with_appearance(MediaContext context,
                                           const hal::AppearancePreference& appearance) noexcept;

} // namespace tessera::ui::style
