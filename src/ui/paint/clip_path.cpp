// SPDX-License-Identifier: MIT

#include "tessera/ui/paint/clip_path.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace tessera::ui::paint::detail {
namespace {

// A length-percentage resolved against one axis. `percent` is the raw number before the `%`, so the
// caller decides which axis it multiplies.
struct Length {
    float value = 0.0f;
    bool percent = false;

    [[nodiscard]] float resolve(float reference) const {
        return percent ? value * reference * 0.01f : value;
    }
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

// Splits on whitespace at paren depth 0, so `inset(10px 20px)` stays one token while its arguments
// are split later by `tokenize_arguments`. Commas are dropped, which is what the CSS grammar allows
// between arguments.
std::vector<std::string> tokenize(std::string_view text) {
    std::vector<std::string> tokens;
    std::string current;
    int depth = 0;
    for (const char character : text) {
        if (character == '(') {
            ++depth;
        } else if (character == ')') {
            --depth;
        }
        if (depth == 0 &&
            (std::isspace(static_cast<unsigned char>(character)) != 0 || character == ',')) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(character);
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

// Splits a function's argument text on whitespace and commas. This is the inner split: the outer
// `tokenize` keeps a whole `name(...)` together so the function can be recognised.
std::vector<std::string> tokenize_arguments(std::string_view text) {
    std::vector<std::string> tokens;
    std::string current;
    for (const char character : text) {
        if (character == ',' || std::isspace(static_cast<unsigned char>(character)) != 0) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(character);
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

// Parses one length-percentage. Returns false for anything else, including `auto` and `calc()`
// (the cascade resolves `calc()` before this point, so a surviving one is a value this build cannot
// read).
bool parse_length(std::string_view token, Length& out) {
    if (token.empty()) {
        return false;
    }
    std::string text(token);
    bool percent = false;
    if (text.back() == '%') {
        percent = true;
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
    out.percent = percent;
    return true;
}

// The `at <position>` clause of `circle()`/`ellipse()`. The supported positions are the four
// keywords and a two-length form; a percentage resolves against the box, a length is absolute.
struct Position {
    Length x;
    Length y;
};

bool parse_position(const std::vector<std::string>& tokens, std::size_t first, Position& out) {
    if (first >= tokens.size()) {
        // No `at` clause: the centre is the box centre, which the caller applies.
        out.x = Length{50.0f, true};
        out.y = Length{50.0f, true};
        return true;
    }
    const std::string& token = tokens[first];
    if (token == "center") {
        out.x = Length{50.0f, true};
        out.y = Length{50.0f, true};
        return first + 1 == tokens.size();
    }
    if (token == "left" || token == "right") {
        out.x = Length{token == "left" ? 0.0f : 100.0f, true};
        if (first + 1 < tokens.size() &&
            (tokens[first + 1] == "top" || tokens[first + 1] == "bottom")) {
            out.y = Length{tokens[first + 1] == "top" ? 0.0f : 100.0f, true};
            return first + 2 == tokens.size();
        }
        out.y = Length{50.0f, true};
        return first + 1 == tokens.size();
    }
    if (token == "top" || token == "bottom") {
        out.y = Length{token == "top" ? 0.0f : 100.0f, true};
        if (first + 1 < tokens.size() &&
            (tokens[first + 1] == "left" || tokens[first + 1] == "right")) {
            out.x = Length{tokens[first + 1] == "left" ? 0.0f : 100.0f, true};
            return first + 2 == tokens.size();
        }
        out.x = Length{50.0f, true};
        return first + 1 == tokens.size();
    }
    // Two lengths: x then y.
    if (!parse_length(token, out.x)) {
        return false;
    }
    if (first + 1 >= tokens.size() || !parse_length(tokens[first + 1], out.y)) {
        return false;
    }
    return first + 2 == tokens.size();
}

// The name and argument text of a functional token: `circle(50%)` -> ("circle", "50%)").
bool split_function(const std::string& token, std::string& name, std::string& arguments) {
    const std::size_t open = token.find('(');
    if (open == std::string::npos || token.back() != ')') {
        return false;
    }
    name = token.substr(0, open);
    arguments = token.substr(open + 1, token.size() - open - 2);
    return true;
}

// `inset( <length-percentage>{1,4} [ round <length-percentage>{1,4} ]? )`.
bool parse_inset(const std::string& arguments, const rendering::rhi::Rect& box, ClipShape& shape) {
    const std::vector<std::string> tokens = tokenize_arguments(arguments);
    if (tokens.empty()) {
        return false;
    }
    std::vector<Length> insets;
    std::vector<Length> radii;
    bool in_radii = false;
    for (const std::string& token : tokens) {
        if (token == "round") {
            in_radii = true;
            continue;
        }
        Length length;
        if (!parse_length(token, length)) {
            return false;
        }
        (in_radii ? radii : insets).push_back(length);
    }
    if (insets.empty() || insets.size() > 4 || radii.size() > 4) {
        return false;
    }
    // The CSS 1-to-4 expansion, in top/right/bottom/left order:
    //   1 value  -> all four sides
    //   2 values -> vertical (top/bottom), horizontal (right/left)
    //   3 values -> top, horizontal (right/left), bottom
    //   4 values -> top, right, bottom, left
    const auto side = [&insets](std::size_t index) {
        switch (insets.size()) {
        case 1:
            return insets[0];
        case 2:
            // index 0 = top, 1 = right, 2 = bottom, 3 = left.
            return insets[index % 2 == 0 ? 0 : 1];
        case 3:
            return index == 0 ? insets[0] : (index == 2 ? insets[2] : insets[1]);
        default:
            return insets[index];
        }
    };
    shape.inset_top = side(0).resolve(box.height);
    shape.inset_right = side(1).resolve(box.width);
    shape.inset_bottom = side(2).resolve(box.height);
    shape.inset_left = side(3).resolve(box.width);

    if (!radii.empty()) {
        // The same 1-to-4 expansion, in TL/TR/BR/BL order.
        const auto corner = [&radii](std::size_t index) {
            switch (radii.size()) {
            case 1:
                return radii[0];
            case 2:
                return radii[index % 2 == 0 ? 0 : 1];
            case 3:
                return index == 0 ? radii[0] : (index == 2 ? radii[2] : radii[1]);
            default:
                return radii[index];
            }
        };
        // A corner radius resolves against the smaller of the two axes, which is the CSS rule for a
        // circular corner and keeps the radius representable.
        const float reference = std::min(box.width, box.height);
        shape.radius_tl = corner(0).resolve(reference);
        shape.radius_tr = corner(1).resolve(reference);
        shape.radius_br = corner(2).resolve(reference);
        shape.radius_bl = corner(3).resolve(reference);
    }
    shape.kind = ClipShape::Kind::Inset;
    return true;
}

// `circle( <length-percentage>? [ at <position> ]? )`.
bool parse_circle(const std::string& arguments, const rendering::rhi::Rect& box, ClipShape& shape) {
    const std::vector<std::string> tokens = tokenize_arguments(arguments);
    std::size_t index = 0;
    Length radius{50.0f, true};
    if (index < tokens.size() && tokens[index] != "at") {
        if (!parse_length(tokens[index], radius)) {
            return false;
        }
        ++index;
    }
    Position position;
    if (index < tokens.size()) {
        if (tokens[index] != "at") {
            return false;
        }
        if (!parse_position(tokens, index + 1, position)) {
            return false;
        }
    } else {
        position.x = Length{50.0f, true};
        position.y = Length{50.0f, true};
    }
    // A circle's radius resolves against the box's smaller side, which is the CSS rule.
    const float reference = std::min(box.width, box.height);
    shape.radius_x = radius.resolve(reference);
    shape.radius_y = shape.radius_x;
    shape.center_x = box.x + position.x.resolve(box.width);
    shape.center_y = box.y + position.y.resolve(box.height);
    shape.kind = ClipShape::Kind::Circle;
    return true;
}

// `ellipse( <length-percentage>{2} [ at <position> ]? )`.
bool parse_ellipse(const std::string& arguments, const rendering::rhi::Rect& box,
                   ClipShape& shape) {
    const std::vector<std::string> tokens = tokenize_arguments(arguments);
    if (tokens.size() < 2) {
        return false;
    }
    Length radius_x;
    Length radius_y;
    if (!parse_length(tokens[0], radius_x) || !parse_length(tokens[1], radius_y)) {
        return false;
    }
    std::size_t index = 2;
    Position position;
    if (index < tokens.size()) {
        if (tokens[index] != "at") {
            return false;
        }
        if (!parse_position(tokens, index + 1, position)) {
            return false;
        }
    } else {
        position.x = Length{50.0f, true};
        position.y = Length{50.0f, true};
    }
    shape.radius_x = radius_x.resolve(box.width);
    shape.radius_y = radius_y.resolve(box.height);
    shape.center_x = box.x + position.x.resolve(box.width);
    shape.center_y = box.y + position.y.resolve(box.height);
    shape.kind = ClipShape::Kind::Ellipse;
    return true;
}

} // namespace

ClipPathParse parse_clip_path(std::string_view value, const rendering::rhi::Rect& box) {
    ClipPathParse result;
    const std::string_view text = trim(value);
    if (text.empty() || text == "none") {
        return result;
    }
    // `polygon()` is slice 5. Reporting it as unsupported rather than as `none` is what lets a
    // caller count the gap instead of silently painting unclipped.
    if (text.starts_with("polygon(") || text.starts_with("path(") || text.starts_with("url(")) {
        result.unsupported = true;
        return result;
    }

    const std::vector<std::string> tokens = tokenize(text);
    if (tokens.size() != 1) {
        result.invalid = true;
        return result;
    }
    std::string name;
    std::string arguments;
    if (!split_function(tokens.front(), name, arguments)) {
        result.invalid = true;
        return result;
    }
    bool ok = false;
    if (name == "inset") {
        ok = parse_inset(arguments, box, result.shape);
    } else if (name == "circle") {
        ok = parse_circle(arguments, box, result.shape);
    } else if (name == "ellipse") {
        ok = parse_ellipse(arguments, box, result.shape);
    } else {
        result.unsupported = true;
        return result;
    }
    if (!ok) {
        result.shape = ClipShape{};
        result.invalid = true;
    }
    return result;
}

rendering::gpu::clip::ClipRecord to_clip_record(const ClipShape& shape,
                                                const rendering::rhi::Rect& box) {
    rendering::gpu::clip::ClipRecord record;
    switch (shape.kind) {
    case ClipShape::Kind::None:
        return record;
    case ClipShape::Kind::Circle:
    case ClipShape::Kind::Ellipse: {
        record.kind = static_cast<std::uint32_t>(rendering::gpu::clip::RecordKind::Ellipse);
        // The record's rect is the ellipse's bounding box, which is what the scissor and the
        // shader's ellipse test both derive from.
        record.x = shape.center_x - shape.radius_x;
        record.y = shape.center_y - shape.radius_y;
        record.width = shape.radius_x * 2.0f;
        record.height = shape.radius_y * 2.0f;
        return record;
    }
    case ClipShape::Kind::Inset: {
        record.kind = static_cast<std::uint32_t>(rendering::gpu::clip::RecordKind::RoundedRect);
        record.x = box.x + shape.inset_left;
        record.y = box.y + shape.inset_top;
        record.width = box.width - shape.inset_left - shape.inset_right;
        record.height = box.height - shape.inset_top - shape.inset_bottom;
        if (record.width <= 0.0f || record.height <= 0.0f) {
            // An inset that consumes the box clips everything away; a zero-size record is what the
            // shader's `has_area` check reads as "nothing is inside".
            record.width = 0.0f;
            record.height = 0.0f;
            return record;
        }
        // The radii are clamped to half the smaller dimension, the same rule `clamp_radii` applies
        // to a border radius, so the corner stays representable.
        const float limit = std::min(record.width, record.height) * 0.5f;
        const auto clamp_radius = [limit](float radius) {
            return std::clamp(radius, 0.0f, std::max(limit, 0.0f));
        };
        record.radii[0] = clamp_radius(shape.radius_tl);
        record.radii[1] = clamp_radius(shape.radius_tr);
        record.radii[2] = clamp_radius(shape.radius_br);
        record.radii[3] = clamp_radius(shape.radius_bl);
        return record;
    }
    }
    return record;
}

} // namespace tessera::ui::paint::detail
