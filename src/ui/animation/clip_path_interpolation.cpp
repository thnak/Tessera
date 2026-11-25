// SPDX-License-Identifier: MIT

#include "clip_path_interpolation.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace tessera::ui::animation {
namespace {

// One length-percentage. A bare number is pixels, which is what the paint-time parser reads it as.
struct Len {
    float value = 0.0f;
    bool percent = false;
};

enum class Kind : std::uint8_t { Inset, Circle, Ellipse, Polygon };

// A shape as a flat list of lengths, laid out per kind:
//   Inset    8 values: top, right, bottom, left, then the radii TL, TR, BR, BL
//   Circle   3 values: radius, centre x, centre y
//   Ellipse  4 values: radius x, radius y, centre x, centre y
//   Polygon  2 values per vertex
struct Syntax {
    Kind kind = Kind::Inset;
    std::vector<Len> values;
    bool even_odd = false;
    // Inset: whether the source had a `round` clause (the output keeps it so a round-trip is
    // stable).
    bool rounded = false;
};

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

// Whitespace-and-comma split of a function's arguments.
std::vector<std::string> words(std::string_view text) {
    std::vector<std::string> tokens;
    std::string current;
    for (const char character : text) {
        if (character == ',' || std::isspace(static_cast<unsigned char>(character)) != 0) {
            if (!current.empty()) {
                tokens.push_back(std::move(current));
                current.clear();
            }
            continue;
        }
        current.push_back(character);
    }
    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

bool parse_len(std::string_view token, Len& out) {
    std::string text(token);
    out.percent = false;
    if (text.empty()) {
        return false;
    }
    if (text.back() == '%') {
        out.percent = true;
        text.pop_back();
    } else if (text.size() >= 2 && text.compare(text.size() - 2, 2, "px") == 0) {
        text.resize(text.size() - 2);
    }
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(value)) {
        return false;
    }
    out.value = static_cast<float>(value);
    return true;
}

// The CSS 1-to-4 expansion shared by the insets and the corner radii.
Len expand(const std::vector<Len>& list, std::size_t index) {
    switch (list.size()) {
    case 1:
        return list[0];
    case 2:
        return list[index % 2 == 0 ? 0 : 1];
    case 3:
        return index == 0 ? list[0] : (index == 2 ? list[2] : list[1]);
    default:
        return list[index];
    }
}

// `at <position>` or nothing, from `tokens[first]`. Keywords become percentages, as the paint-time
// parser reads them.
bool parse_position(const std::vector<std::string>& tokens, std::size_t first, Len& x, Len& y) {
    x = Len{50.0f, true};
    y = Len{50.0f, true};
    if (first >= tokens.size()) {
        return true;
    }
    const std::string& token = tokens[first];
    const auto is = [&tokens](std::size_t index, const char* word) {
        return index < tokens.size() && tokens[index] == word;
    };
    if (token == "center") {
        return first + 1 == tokens.size();
    }
    if (token == "left" || token == "right") {
        x = Len{token == "left" ? 0.0f : 100.0f, true};
        if (is(first + 1, "top") || is(first + 1, "bottom")) {
            y = Len{tokens[first + 1] == "top" ? 0.0f : 100.0f, true};
            return first + 2 == tokens.size();
        }
        return first + 1 == tokens.size();
    }
    if (token == "top" || token == "bottom") {
        y = Len{token == "top" ? 0.0f : 100.0f, true};
        if (is(first + 1, "left") || is(first + 1, "right")) {
            x = Len{tokens[first + 1] == "left" ? 0.0f : 100.0f, true};
            return first + 2 == tokens.size();
        }
        return first + 1 == tokens.size();
    }
    return parse_len(token, x) && first + 1 < tokens.size() && parse_len(tokens[first + 1], y) &&
           first + 2 == tokens.size();
}

bool parse_inset(std::string_view arguments, Syntax& out) {
    std::vector<Len> insets;
    std::vector<Len> radii;
    bool in_radii = false;
    for (const std::string& token : words(arguments)) {
        if (token == "round") {
            in_radii = true;
            out.rounded = true;
            continue;
        }
        Len length;
        if (!parse_len(token, length)) {
            return false;
        }
        (in_radii ? radii : insets).push_back(length);
    }
    if (insets.empty() || insets.size() > 4 || radii.size() > 4 || (in_radii && radii.empty())) {
        return false;
    }
    out.kind = Kind::Inset;
    for (std::size_t index = 0; index < 4; ++index) {
        out.values.push_back(expand(insets, index));
    }
    for (std::size_t index = 0; index < 4; ++index) {
        out.values.push_back(radii.empty() ? Len{} : expand(radii, index));
    }
    return true;
}

bool parse_circle(std::string_view arguments, Syntax& out) {
    const std::vector<std::string> tokens = words(arguments);
    std::size_t index = 0;
    Len radius{50.0f, true};
    if (index < tokens.size() && tokens[index] != "at") {
        if (!parse_len(tokens[index], radius)) {
            return false;
        }
        ++index;
    }
    Len x;
    Len y;
    if (index < tokens.size()) {
        if (tokens[index] != "at" || !parse_position(tokens, index + 1, x, y)) {
            return false;
        }
    } else {
        x = Len{50.0f, true};
        y = Len{50.0f, true};
    }
    out.kind = Kind::Circle;
    out.values = {radius, x, y};
    return true;
}

bool parse_ellipse(std::string_view arguments, Syntax& out) {
    const std::vector<std::string> tokens = words(arguments);
    Len radius_x;
    Len radius_y;
    if (tokens.size() < 2 || !parse_len(tokens[0], radius_x) || !parse_len(tokens[1], radius_y)) {
        return false;
    }
    Len x;
    Len y;
    if (tokens.size() > 2) {
        if (tokens[2] != "at" || !parse_position(tokens, 3, x, y)) {
            return false;
        }
    } else {
        x = Len{50.0f, true};
        y = Len{50.0f, true};
    }
    out.kind = Kind::Ellipse;
    out.values = {radius_x, radius_y, x, y};
    return true;
}

bool parse_polygon(std::string_view arguments, Syntax& out) {
    std::vector<std::string> parts;
    std::string current;
    for (const char character : arguments) {
        if (character == ',') {
            parts.push_back(std::move(current));
            current.clear();
            continue;
        }
        current.push_back(character);
    }
    parts.push_back(std::move(current));
    std::size_t first = 0;
    const std::string_view head = trim(parts.front());
    if (head == "nonzero" || head == "evenodd") {
        out.even_odd = head == "evenodd";
        first = 1;
    }
    if (first >= parts.size()) {
        return false;
    }
    for (std::size_t index = first; index < parts.size(); ++index) {
        const std::vector<std::string> pair = words(parts[index]);
        Len x;
        Len y;
        if (pair.size() != 2 || !parse_len(pair[0], x) || !parse_len(pair[1], y)) {
            return false;
        }
        out.values.push_back(x);
        out.values.push_back(y);
    }
    out.kind = Kind::Polygon;
    return true;
}

bool parse_shape(std::string_view value, Syntax& out) {
    const std::string_view text = trim(value);
    const std::size_t open = text.find('(');
    if (open == std::string_view::npos || text.back() != ')') {
        return false;
    }
    const std::string_view name = trim(text.substr(0, open));
    const std::string_view arguments = text.substr(open + 1, text.size() - open - 2);
    // A nested function (`calc()`, `var()`) is not something this reader interpolates.
    if (arguments.find('(') != std::string_view::npos) {
        return false;
    }
    if (name == "inset") {
        return parse_inset(arguments, out);
    }
    if (name == "circle") {
        return parse_circle(arguments, out);
    }
    if (name == "ellipse") {
        return parse_ellipse(arguments, out);
    }
    if (name == "polygon") {
        return parse_polygon(arguments, out);
    }
    return false;
}

// One component at progress `t`. A zero takes the other side's unit, so `0` against `10%` blends;
// two different units that are both non-zero cannot (CSS would produce a `calc()`).
bool blend(const Len& from, const Len& to, double t, Len& out) {
    bool percent = from.percent;
    if (from.percent != to.percent) {
        if (from.value == 0.0f) {
            percent = to.percent;
        } else if (to.value != 0.0f) {
            return false;
        }
    }
    out.percent = percent;
    out.value = static_cast<float>(from.value + (to.value - from.value) * t);
    return true;
}

// `%.4f` with trailing zeros trimmed, so an interpolated value is stable text.
std::string format(const Len& length) {
    float value = length.value;
    if (std::abs(value) < 0.00005f) {
        value = 0.0f;
    }
    char buffer[48] = {};
    std::snprintf(buffer, sizeof(buffer), "%.4f", static_cast<double>(value));
    std::string text(buffer);
    while (!text.empty() && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    if (text.empty()) {
        text = "0";
    }
    return text + (length.percent ? "%" : "px");
}

std::string join(const std::vector<Len>& values, std::size_t first, std::size_t count) {
    std::string text;
    for (std::size_t index = 0; index < count; ++index) {
        text += (index == 0 ? "" : " ") + format(values[first + index]);
    }
    return text;
}

// Radii cannot be negative, so extrapolation past the ends stops at zero.
void clamp_radius(Len& length) {
    length.value = std::max(length.value, 0.0f);
}

std::string write(const Syntax& shape) {
    const std::vector<Len>& v = shape.values;
    switch (shape.kind) {
    case Kind::Inset: {
        std::string text = "inset(" + join(v, 0, 4);
        if (shape.rounded) {
            text += " round " + join(v, 4, 4);
        }
        return text + ")";
    }
    case Kind::Circle:
        return "circle(" + format(v[0]) + " at " + join(v, 1, 2) + ")";
    case Kind::Ellipse:
        return "ellipse(" + join(v, 0, 2) + " at " + join(v, 2, 2) + ")";
    case Kind::Polygon: {
        std::string text = shape.even_odd ? "polygon(evenodd, " : "polygon(";
        for (std::size_t index = 0; index + 1 < v.size(); index += 2) {
            text += (index == 0 ? "" : ", ") + join(v, index, 2);
        }
        return text + ")";
    }
    }
    return {};
}

} // namespace

std::optional<std::string> interpolate_clip_path(std::string_view from, std::string_view to,
                                                 double t) {
    Syntax lhs;
    Syntax rhs;
    if (!parse_shape(from, lhs) || !parse_shape(to, rhs)) {
        return std::nullopt;
    }
    if (lhs.kind != rhs.kind || lhs.values.size() != rhs.values.size() ||
        lhs.even_odd != rhs.even_odd) {
        return std::nullopt;
    }
    Syntax result;
    result.kind = lhs.kind;
    result.even_odd = lhs.even_odd;
    result.rounded = lhs.rounded || rhs.rounded;
    result.values.resize(lhs.values.size());
    for (std::size_t index = 0; index < lhs.values.size(); ++index) {
        if (!blend(lhs.values[index], rhs.values[index], t, result.values[index])) {
            return std::nullopt;
        }
    }
    switch (result.kind) {
    case Kind::Inset:
        for (std::size_t index = 4; index < 8; ++index) {
            clamp_radius(result.values[index]);
        }
        break;
    case Kind::Circle:
        clamp_radius(result.values[0]);
        break;
    case Kind::Ellipse:
        clamp_radius(result.values[0]);
        clamp_radius(result.values[1]);
        break;
    case Kind::Polygon:
        break;
    }
    return write(result);
}

} // namespace tessera::ui::animation
