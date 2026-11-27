// SPDX-License-Identifier: MIT

#include "gradient_bodies.hpp"
#include "gradient_internal.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace tessera::ui::style::detail {
namespace {

// `circle` / `ellipse`, or nullopt when the token is not a shape keyword.
std::optional<RadialShape> parse_shape(std::string_view text) {
    const std::string token = lower(trim(text));
    if (token == "circle") {
        return RadialShape::Circle;
    }
    if (token == "ellipse") {
        return RadialShape::Ellipse;
    }
    return std::nullopt;
}

// The four size keywords of CSS Images 3 section 3.2.1.
std::optional<RadialSize> parse_size_keyword(std::string_view text) {
    const std::string token = lower(trim(text));
    if (token == "closest-side") {
        return RadialSize::ClosestSide;
    }
    if (token == "closest-corner") {
        return RadialSize::ClosestCorner;
    }
    if (token == "farthest-side") {
        return RadialSize::FarthestSide;
    }
    if (token == "farthest-corner") {
        return RadialSize::FarthestCorner;
    }
    return std::nullopt;
}

// One `<position>` component: a percentage of the box, or a px length. A px length needs the box to
// resolve, which the parser does not have, so it is rejected rather than guessed at. The keywords
// `left`/`right`/`top`/`bottom`/`center` are the CSS shorthands for 0 %/100 %/50 %.
std::optional<float> parse_position_component(std::string_view text, bool horizontal) {
    const std::string token = lower(trim(text));
    if (token == "center") {
        return 0.5f;
    }
    if (horizontal) {
        if (token == "left") {
            return 0.0f;
        }
        if (token == "right") {
            return 1.0f;
        }
    } else {
        if (token == "top") {
            return 0.0f;
        }
        if (token == "bottom") {
            return 1.0f;
        }
    }
    return parse_percentage(token);
}

// `at <position>`: one or two components, `x y` or a single value meaning both axes (CSS
// Images 3 section 3.2.1). Returns false when the position is outside what this grammar resolves.
bool parse_position(const std::vector<std::string_view>& tokens, RadialPosition& out) {
    if (tokens.empty() || tokens.size() > 2) {
        return false;
    }
    if (tokens.size() == 1) {
        const std::optional<float> both = parse_position_component(tokens[0], true);
        if (!both) {
            return false;
        }
        out.x = *both;
        out.y = *both;
        return true;
    }
    const std::optional<float> x = parse_position_component(tokens[0], true);
    const std::optional<float> y = parse_position_component(tokens[1], false);
    if (!x || !y) {
        return false;
    }
    out.x = *x;
    out.y = *y;
    return true;
}

// The prelude of a radial gradient: `[<shape> || <size>]? [at <position>]?`. Everything before the
// first colour stop. Returns false when a token is outside the grammar.
bool parse_prelude(const std::vector<std::string_view>& tokens, RadialGradientValue& out) {
    std::size_t index = 0;
    bool have_shape = false;
    bool have_size = false;
    while (index < tokens.size()) {
        const std::string token = lower(trim(tokens[index]));
        if (token == "at") {
            ++index;
            std::vector<std::string_view> position(
                tokens.begin() + static_cast<std::ptrdiff_t>(index), tokens.end());
            return parse_position(position, out.position);
        }
        if (!have_shape) {
            if (const std::optional<RadialShape> shape = parse_shape(token)) {
                out.shape = *shape;
                have_shape = true;
                ++index;
                continue;
            }
        }
        if (!have_size) {
            if (const std::optional<RadialSize> size = parse_size_keyword(token)) {
                out.size = *size;
                have_size = true;
                ++index;
                continue;
            }
            // Explicit radii: one length (a circle) or two (an ellipse). A percentage radius needs
            // the box, so it is outside this grammar.
            if (const std::optional<float> first = parse_px_length(token)) {
                if (index + 1 < tokens.size()) {
                    if (const std::optional<float> second = parse_px_length(tokens[index + 1])) {
                        out.size = RadialSize::Explicit;
                        out.radius_x = *first;
                        out.radius_y = *second;
                        have_size = true;
                        index += 2;
                        continue;
                    }
                }
                out.size = RadialSize::Explicit;
                out.radius_x = *first;
                out.radius_y = *first;
                have_size = true;
                ++index;
                continue;
            }
        }
        return false;
    }
    // A prelude with no `at` is the default centre, which the struct already holds.
    return true;
}

} // namespace

bool parse_radial_body(std::string_view inner, RadialGradientValue& out) {
    const std::vector<std::string_view> parts = split_commas(inner);
    if (parts.empty()) {
        return false;
    }

    // The prelude is the first comma-separated part when it is not itself a colour stop. A radial
    // gradient with no prelude starts at its first stop (`radial-gradient(red, blue)`).
    std::size_t first_stop = 0;
    if (!looks_like_stop(parts[0])) {
        const std::vector<std::string_view> tokens = split_spaces(parts[0]);
        if (!parse_prelude(tokens, out)) {
            return false;
        }
        first_stop = 1;
    }

    return parse_stop_list(parts, first_stop, StopDomain::Line, out.stops, out.positions,
                           out.stop_count);
}

} // namespace tessera::ui::style::detail
