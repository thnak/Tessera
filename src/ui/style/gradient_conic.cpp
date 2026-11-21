// SPDX-License-Identifier: MIT

// The `conic-gradient()` half of the gradient grammar (issue #617).
//
// The prelude is `[from <angle>]? [at <position>]?`, both optional and in that order, which is the
// CSS Images 3 section 3.3.1 grammar. Everything after it is the ordinary stop list the linear and
// radial halves share, so a conic gradient reads its stops exactly as they do.

#include "gradient_bodies.hpp"
#include "gradient_internal.hpp"

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace tessera::ui::style::detail {
namespace {

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

// `at <position>`: one or two components, the second defaulting to `center`. The struct already
// holds the default centre, so a prelude with no `at` needs no work.
bool parse_position(const std::vector<std::string_view>& tokens, std::size_t index,
                    RadialPosition& out) {
    if (index >= tokens.size()) {
        return false;
    }
    const std::optional<float> x = parse_position_component(tokens[index], true);
    if (!x) {
        return false;
    }
    out.x = *x;
    if (index + 1 < tokens.size()) {
        const std::optional<float> y = parse_position_component(tokens[index + 1], false);
        if (!y) {
            return false;
        }
        out.y = *y;
        return index + 2 == tokens.size();
    }
    return true;
}

// The prelude: `[from <angle>]? [at <position>]?`. Returns false when a token is outside it.
bool parse_prelude(const std::vector<std::string_view>& tokens, ConicGradientValue& out) {
    std::size_t index = 0;
    if (index < tokens.size() && lower(trim(tokens[index])) == "from") {
        if (index + 1 >= tokens.size()) {
            return false;
        }
        const std::optional<float> angle = parse_angle(tokens[index + 1]);
        if (!angle) {
            return false;
        }
        // The sweep is periodic, so the authored angle is normalised into [0, 360).
        out.from_angle_degrees = std::fmod(std::fmod(*angle, 360.0f) + 360.0f, 360.0f);
        index += 2;
    }
    if (index < tokens.size() && lower(trim(tokens[index])) == "at") {
        return parse_position(tokens, index + 1, out.position);
    }
    return index == tokens.size();
}

// True when `text` is the start of a colour stop rather than a prelude token. A stop is a colour,
// and every colour spelling either starts with `#` or is a function/identifier; the prelude's own
// keywords are all handled before this is consulted.
bool looks_like_stop(std::string_view text) {
    const std::string token = lower(trim(text));
    if (token.empty()) {
        return false;
    }
    if (token.front() == '#') {
        return true;
    }
    return parse_color(token).ok;
}

} // namespace

bool parse_conic_body(std::string_view inner, ConicGradientValue& out) {
    const std::vector<std::string_view> parts = split_commas(inner);
    if (parts.empty()) {
        return false;
    }

    // The prelude is the first comma-separated part when it is not itself a colour stop. A conic
    // gradient with no prelude starts at its first stop (`conic-gradient(red, blue)`).
    std::size_t first_stop = 0;
    if (!looks_like_stop(parts[0])) {
        const std::vector<std::string_view> tokens = split_spaces(parts[0]);
        if (!parse_prelude(tokens, out)) {
            return false;
        }
        first_stop = 1;
    }

    const std::size_t count = parts.size() - first_stop;
    if (count < 2 || count > kMaxGradientStops) {
        return false;
    }
    std::array<std::optional<float>, kMaxGradientStops> positions;
    for (std::size_t index = 0; index < count; ++index) {
        if (!parse_stop(parts[first_stop + index], out.stops[index].color, positions[index])) {
            return false;
        }
    }
    out.stop_count = count;
    fix_up_stop_offsets(positions, count);
    for (std::size_t index = 0; index < count; ++index) {
        out.stops[index].offset = positions[index].value();
    }
    return true;
}

} // namespace tessera::ui::style::detail
