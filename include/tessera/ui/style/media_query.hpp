// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/window_backdrop.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style {

enum class ColorScheme {
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
    hal::BackdropMaterial window_backdrop = hal::BackdropMaterial::None;
};

enum class MediaFeature {
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

enum class MediaComparison {
    Equal,
    Min,
    Max,
};

struct MediaCondition {
    MediaFeature feature = MediaFeature::Width;
    MediaComparison comparison = MediaComparison::Equal;
    std::string value;
};

struct MediaQuery {
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

} // namespace tessera::ui::style
