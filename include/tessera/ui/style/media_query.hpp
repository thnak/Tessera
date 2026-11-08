// SPDX-License-Identifier: MIT

#pragma once

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
};

enum class MediaFeature {
    Width,
    Height,
    Resolution,
    Orientation,
    PrefersColorScheme,
    PrefersReducedMotion,
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
