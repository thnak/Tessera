// SPDX-License-Identifier: MIT

#include "media_query_internal.hpp"
#include "tessera/detail/text_util.hpp"
#include "tessera/ui/style/media_query.hpp"
#include "tessera/ui/style/viewport_units.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::style {

using tessera::detail::lower;
using tessera::detail::split_top_level;
namespace {

std::string trim(std::string_view value) {
    return std::string(tessera::detail::trim(value));
}

bool starts_with_word(std::string_view text, std::string_view word) {
    if (text.size() < word.size() || text.substr(0, word.size()) != word) {
        return false;
    }
    return text.size() == word.size() ||
           std::isspace(static_cast<unsigned char>(text[word.size()])) != 0;
}

bool parse_number_value(std::string_view text, double& value, std::string_view& unit) {
    std::size_t index = 0;
    while (index < text.size() &&
           (std::isdigit(static_cast<unsigned char>(text[index])) != 0 || text[index] == '.' ||
            text[index] == '+' || text[index] == '-')) {
        ++index;
    }
    if (index == 0) {
        return false;
    }
    const std::string number(text.substr(0, index));
    char* end = nullptr;
    value = std::strtod(number.c_str(), &end);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    unit = text.substr(index);
    return true;
}

bool parse_length(std::string_view text, const MediaContext& context, double& pixels) {
    double number = 0.0;
    std::string_view unit;
    if (!parse_number_value(text, number, unit)) {
        return false;
    }
    if (unit == "px" || unit.empty()) {
        pixels = number;
        return true;
    }
    if (unit == "rem" || unit == "em") {
        pixels = number * 16.0;
        return true;
    }
    // Every viewport unit comes from the one shared table (issue #691), so `@media` accepts exactly
    // the units layout and grid do - including `vmin`, `vmax` and the small/large/dynamic family.
    ViewportSize size = ViewportSize::Default;
    ViewportAxis axis = ViewportAxis::Width;
    if (parse_viewport_unit(unit, size, axis)) {
        ViewportUnits viewport;
        viewport.width = context.viewport_width;
        viewport.height = context.viewport_height;
        pixels = resolve_viewport_length(ViewportLength{size, axis, number}, viewport);
        return true;
    }
    return false;
}

bool parse_resolution(std::string_view text, double& dpi) {
    double number = 0.0;
    std::string_view unit;
    if (!parse_number_value(text, number, unit)) {
        return false;
    }
    if (unit == "dpi") {
        dpi = number;
        return true;
    }
    if (unit == "dppx") {
        dpi = number * 96.0;
        return true;
    }
    if (unit == "dpcm") {
        dpi = number * 2.54;
        return true;
    }
    return false;
}

// Maps a feature name (with any `min-`/`max-` prefix already stripped) to its enum. Returns false
// for a name the engine does not implement, which keeps today's behaviour: the query is false and a
// diagnostic names it, rather than silently accepting.
bool compare_number(double actual, double expected, MediaComparison comparison) {
    switch (comparison) {
    case MediaComparison::Min:
    case MediaComparison::GreaterEqual:
        return actual >= expected;
    case MediaComparison::Max:
    case MediaComparison::LessEqual:
        return actual <= expected;
    case MediaComparison::Greater:
        return actual > expected;
    case MediaComparison::Less:
        return actual < expected;
    case MediaComparison::Equal:
        return std::abs(actual - expected) < 0.5;
    }
    return false;
}

// An `aspect-ratio` value as an exact integer ratio. CSS writes it `16/9` or `16 / 9`; a bare
// number is `n/1`. Comparing as integers rather than as a float is what makes `16/9` exact: a
// viewport of 1600x900 must match `aspect-ratio: 16/9` and a float division would leave a rounding
// residue.
struct Ratio {
    long long width = 1;
    long long height = 1;
};

bool parse_ratio(std::string_view text, Ratio& ratio) {
    const std::string token = trim(text);
    if (token.empty()) {
        return false;
    }
    const std::size_t slash = token.find('/');
    const std::string_view left = slash == std::string::npos
                                      ? std::string_view(token)
                                      : std::string_view(token).substr(0, slash);
    const std::string_view right = slash == std::string::npos
                                       ? std::string_view("1")
                                       : std::string_view(token).substr(slash + 1);
    const auto parse_positive = [](std::string_view part, long long& out) {
        const std::string trimmed = trim(part);
        if (trimmed.empty()) {
            return false;
        }
        char* end = nullptr;
        const long long value = std::strtoll(trimmed.c_str(), &end, 10);
        if (end == nullptr || *end != '\0' || value <= 0) {
            return false;
        }
        out = value;
        return true;
    };
    return parse_positive(left, ratio.width) && parse_positive(right, ratio.height);
}

// The viewport's own ratio, reduced so the cross-multiplication stays small.
Ratio viewport_ratio(const MediaContext& context) {
    Ratio ratio;
    ratio.width = static_cast<long long>(std::llround(context.viewport_width));
    ratio.height = static_cast<long long>(std::llround(context.viewport_height));
    if (ratio.width <= 0 || ratio.height <= 0) {
        ratio.width = 1;
        ratio.height = 1;
    }
    return ratio;
}

// `actual op expected` for two ratios, cross-multiplied so no division happens.
bool compare_ratio(const Ratio& actual, const Ratio& expected, MediaComparison comparison) {
    const long long left = actual.width * expected.height;
    const long long right = expected.width * actual.height;
    switch (comparison) {
    case MediaComparison::Min:
    case MediaComparison::GreaterEqual:
        return left >= right;
    case MediaComparison::Max:
    case MediaComparison::LessEqual:
        return left <= right;
    case MediaComparison::Greater:
        return left > right;
    case MediaComparison::Less:
        return left < right;
    case MediaComparison::Equal:
        return left == right;
    }
    return false;
}

// The `window-backdrop` grammar of glass-v1.md section 2: `none | mica | acrylic | blur` on the
// resolved material. Kept local (a keyword comparison, not a HAL call) so the style library has no
// link dependency on the window HAL; `auto` is a request and therefore never matches.
bool backdrop_keyword_matches(std::string_view value, hal::BackdropMaterial material) {
    switch (material) {
    case hal::BackdropMaterial::NoMaterial:
        return value == "none";
    case hal::BackdropMaterial::Mica:
        return value == "mica";
    case hal::BackdropMaterial::Acrylic:
        return value == "acrylic";
    case hal::BackdropMaterial::Blur:
        return value == "blur";
    }
    return false;
}

bool evaluate_condition(const MediaCondition& condition, const MediaContext& context) {
    const std::string value = lower(trim(condition.value));
    // The two-sided range form applies to every range feature: `600px <= width <= 900px`. The lower
    // bound is `condition.value`/`comparison`, the upper `upper_value`/`upper_comparison`.
    const auto within_upper = [&condition](double actual, auto&& parse) {
        if (condition.upper_value.empty()) {
            return true;
        }
        double upper = 0.0;
        return parse(lower(trim(condition.upper_value)), upper) &&
               compare_number(actual, upper, condition.upper_comparison);
    };
    switch (condition.feature) {
    case MediaFeature::Width: {
        double expected = 0.0;
        return parse_length(value, context, expected) &&
               compare_number(context.viewport_width, expected, condition.comparison) &&
               within_upper(context.viewport_width, [&context](std::string_view text, double& out) {
                   return parse_length(text, context, out);
               });
    }
    case MediaFeature::Height: {
        double expected = 0.0;
        return parse_length(value, context, expected) &&
               compare_number(context.viewport_height, expected, condition.comparison) &&
               within_upper(context.viewport_height,
                            [&context](std::string_view text, double& out) {
                                return parse_length(text, context, out);
                            });
    }
    case MediaFeature::Resolution: {
        double expected = 0.0;
        return parse_resolution(value, expected) &&
               compare_number(context.resolution_dpi, expected, condition.comparison) &&
               within_upper(context.resolution_dpi, [](std::string_view text, double& out) {
                   return parse_resolution(text, out);
               });
    }
    case MediaFeature::Orientation:
        if (value == "landscape") {
            return context.viewport_width >= context.viewport_height;
        }
        if (value == "portrait") {
            return context.viewport_width < context.viewport_height;
        }
        return false;
    case MediaFeature::PrefersColorScheme:
        if (value == "light") {
            return context.color_scheme == ColorScheme::Light;
        }
        if (value == "dark") {
            return context.color_scheme == ColorScheme::Dark;
        }
        return false;
    case MediaFeature::PrefersReducedMotion:
        if (value == "reduce") {
            return context.prefers_reduced_motion;
        }
        if (value == "no-preference") {
            return !context.prefers_reduced_motion;
        }
        return false;
    case MediaFeature::WindowBackdrop:
        // Unknown keywords (including `auto`) do not match, exactly like an unknown `orientation`
        // value. `none` matches only the resolved "no material" answer, so a stylesheet can carry
        // its own opaque fallback.
        return backdrop_keyword_matches(value, context.window_backdrop);
    case MediaFeature::AspectRatio: {
        Ratio expected;
        if (!parse_ratio(value, expected)) {
            return false;
        }
        const Ratio actual = viewport_ratio(context);
        if (!compare_ratio(actual, expected, condition.comparison)) {
            return false;
        }
        // The two-sided form: `600/1000 <= aspect-ratio <= 1600/900`.
        if (condition.upper_value.empty()) {
            return true;
        }
        Ratio upper;
        return parse_ratio(condition.upper_value, upper) &&
               compare_ratio(actual, upper, condition.upper_comparison);
    }
    case MediaFeature::Hover:
        if (value == "hover") {
            return context.hover == MediaHover::Hover;
        }
        if (value == "none") {
            return context.hover == MediaHover::None;
        }
        return false;
    case MediaFeature::AnyHover:
        if (value == "hover") {
            return context.any_hover == MediaHover::Hover;
        }
        if (value == "none") {
            return context.any_hover == MediaHover::None;
        }
        return false;
    case MediaFeature::Pointer:
        if (value == "fine") {
            return context.pointer == MediaPointer::Fine;
        }
        if (value == "coarse") {
            return context.pointer == MediaPointer::Coarse;
        }
        if (value == "none") {
            return context.pointer == MediaPointer::None;
        }
        return false;
    case MediaFeature::AnyPointer:
        if (value == "fine") {
            return context.any_pointer == MediaPointer::Fine;
        }
        if (value == "coarse") {
            return context.any_pointer == MediaPointer::Coarse;
        }
        if (value == "none") {
            return context.any_pointer == MediaPointer::None;
        }
        return false;
    }
    return false;
}

} // namespace

MediaQueryParseResult parse_media_query_list(std::string_view text) {
    MediaQueryParseResult result;
    for (const std::string_view part : split_top_level(text, ',')) {
        const std::string query_text = trim(part);
        if (query_text.empty()) {
            continue;
        }
        MediaQuery query;
        if (!parse_query(query_text, query)) {
            result.ok = false;
            result.error_offset = 0;
            continue;
        }
        result.list.queries.push_back(std::move(query));
    }
    if (result.list.queries.empty()) {
        result.ok = false;
    }
    return result;
}

bool evaluate(const MediaQuery& query, const MediaContext& context) {
    bool matched = query.type.empty() || query.type == "all" || query.type == "screen";
    for (const MediaCondition& condition : query.conditions) {
        matched = matched && evaluate_condition(condition, context);
    }
    return query.negated ? !matched : matched;
}

bool evaluate(const MediaQueryList& queries, const MediaContext& context) {
    for (const MediaQuery& query : queries.queries) {
        if (evaluate(query, context)) {
            return true;
        }
    }
    return false;
}

MediaContext with_appearance(MediaContext context,
                             const hal::AppearancePreference& appearance) noexcept {
    // The HAL has already decided what the preference MEANS (its `resolve_color_scheme` applies the
    // documented light fallback and records it as an assumption), so the stylesheet sees one value
    // and never sees a D-Bus or registry detail. `prefers-reduced-motion` is the flag itself.
    context.color_scheme = hal::resolve_color_scheme(appearance.color_scheme).dark
                               ? ColorScheme::Dark
                               : ColorScheme::Light;
    context.prefers_reduced_motion = appearance.reduced_motion;
    return context;
}

} // namespace tessera::ui::style
