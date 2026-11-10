// SPDX-License-Identifier: MIT

#include "tessera/ui/style/transform.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace tessera::ui::style {
namespace {

using rendering::rhi::Mat3;

constexpr float kPi = 3.14159265358979323846f;

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n\f");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n\f");
    return text.substr(first, last - first + 1);
}

std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool parse_float(std::string_view text, float& value) {
    const std::string token(trim(text));
    if (token.empty()) {
        return false;
    }
    char* end = nullptr;
    const float parsed = std::strtof(token.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

// `px`, `%` (of `reference`) or a bare 0.
bool parse_length(std::string_view text, float reference, float& px) {
    const std::string token = lower(trim(text));
    const std::string_view view = token;
    float value = 0.0f;
    if (ends_with(view, "px")) {
        if (!parse_float(view.substr(0, view.size() - 2), value)) {
            return false;
        }
        px = value;
        return true;
    }
    if (ends_with(view, "%")) {
        if (!parse_float(view.substr(0, view.size() - 1), value)) {
            return false;
        }
        px = reference * value / 100.0f;
        return true;
    }
    if (parse_float(view, value) && value == 0.0f) {
        px = 0.0f;
        return true;
    }
    return false;
}

bool parse_angle(std::string_view text, float& radians) {
    const std::string token = lower(trim(text));
    const std::string_view view = token;
    float value = 0.0f;
    struct Unit {
        std::string_view suffix;
        float to_radians;
    };
    // "grad" and "rad" share a suffix: "grad" must be tested before "rad".
    constexpr Unit kUnits[] = {
        {"grad", kPi / 200.0f}, {"deg", kPi / 180.0f}, {"turn", 2.0f * kPi}, {"rad", 1.0f}};
    for (const Unit& unit : kUnits) {
        if (ends_with(view, unit.suffix)) {
            if (!parse_float(view.substr(0, view.size() - unit.suffix.size()), value)) {
                return false;
            }
            radians = value * unit.to_radians;
            return true;
        }
    }
    if (parse_float(view, value) && value == 0.0f) {
        radians = 0.0f;
        return true;
    }
    return false;
}

std::vector<std::string_view> split_arguments(std::string_view text) {
    std::vector<std::string_view> arguments;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == ',') {
            arguments.push_back(trim(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    return arguments;
}

// CSS matrix(a, b, c, d, e, f): x' = a x + c y + e, y' = b x + d y + f.
Mat3 matrix(float a, float b, float c, float d, float e, float f) {
    Mat3 result;
    result.m[0] = a;
    result.m[1] = c;
    result.m[2] = e;
    result.m[3] = b;
    result.m[4] = d;
    result.m[5] = f;
    return result;
}

bool parse_translate(const std::string& name, const std::vector<std::string_view>& args,
                     float width, float height, Mat3& out) {
    float x = 0.0f;
    float y = 0.0f;
    if (name == "translate") {
        if (args.empty() || args.size() > 2 || !parse_length(args[0], width, x) ||
            (args.size() == 2 && !parse_length(args[1], height, y))) {
            return false;
        }
    } else if (name == "translatex") {
        if (args.size() != 1 || !parse_length(args[0], width, x)) {
            return false;
        }
    } else {
        if (args.size() != 1 || !parse_length(args[0], height, y)) {
            return false;
        }
    }
    out = Mat3::translation(x, y);
    return true;
}

bool parse_scale(const std::string& name, const std::vector<std::string_view>& args, Mat3& out) {
    float x = 1.0f;
    float y = 1.0f;
    if (name == "scale") {
        if (args.empty() || args.size() > 2 || !parse_float(args[0], x)) {
            return false;
        }
        y = x;
        if (args.size() == 2 && !parse_float(args[1], y)) {
            return false;
        }
    } else if (name == "scalex") {
        if (args.size() != 1 || !parse_float(args[0], x)) {
            return false;
        }
    } else {
        if (args.size() != 1 || !parse_float(args[0], y)) {
            return false;
        }
    }
    out = Mat3::scale(x, y);
    return true;
}

bool parse_skew(const std::string& name, const std::vector<std::string_view>& args, Mat3& out) {
    float x = 0.0f;
    float y = 0.0f;
    if (name == "skew") {
        if (args.empty() || args.size() > 2 || !parse_angle(args[0], x) ||
            (args.size() == 2 && !parse_angle(args[1], y))) {
            return false;
        }
    } else if (name == "skewx") {
        if (args.size() != 1 || !parse_angle(args[0], x)) {
            return false;
        }
    } else {
        if (args.size() != 1 || !parse_angle(args[0], y)) {
            return false;
        }
    }
    out = matrix(1.0f, std::tan(y), std::tan(x), 1.0f, 0.0f, 0.0f);
    return true;
}

bool parse_function(const std::string& name, const std::vector<std::string_view>& args, float width,
                    float height, Mat3& out) {
    if (name == "translate" || name == "translatex" || name == "translatey") {
        return parse_translate(name, args, width, height, out);
    }
    if (name == "scale" || name == "scalex" || name == "scaley") {
        return parse_scale(name, args, out);
    }
    if (name == "skew" || name == "skewx" || name == "skewy") {
        return parse_skew(name, args, out);
    }
    if (name == "rotate") {
        float radians = 0.0f;
        if (args.size() != 1 || !parse_angle(args[0], radians)) {
            return false;
        }
        out = Mat3::rotation(radians);
        return true;
    }
    if (name == "matrix") {
        float v[6] = {};
        if (args.size() != 6) {
            return false;
        }
        for (std::size_t i = 0; i < 6; ++i) {
            if (!parse_float(args[i], v[i])) {
                return false;
            }
        }
        out = matrix(v[0], v[1], v[2], v[3], v[4], v[5]);
        return true;
    }
    return false;
}

// One origin component: a keyword, or a px/% length of `reference`.
bool parse_origin_component(const std::string& token, float reference, bool horizontal, float& px) {
    if (token == "center") {
        px = reference * 0.5f;
        return true;
    }
    if (token == (horizontal ? "left" : "top")) {
        px = 0.0f;
        return true;
    }
    if (token == (horizontal ? "right" : "bottom")) {
        px = reference;
        return true;
    }
    return parse_length(token, reference, px);
}

bool resolve_origin(std::string_view origin, float width, float height, float& ox, float& oy) {
    std::vector<std::string> parts;
    std::size_t start = std::string_view::npos;
    for (std::size_t i = 0; i <= origin.size(); ++i) {
        const bool space =
            i == origin.size() || std::isspace(static_cast<unsigned char>(origin[i])) != 0;
        if (!space && start == std::string_view::npos) {
            start = i;
        } else if (space && start != std::string_view::npos) {
            parts.push_back(lower(origin.substr(start, i - start)));
            start = std::string_view::npos;
        }
    }
    if (parts.empty()) {
        ox = width * 0.5f;
        oy = height * 0.5f;
        return true;
    }
    if (parts.size() > 2) {
        return false;
    }
    if (parts.size() == 1) {
        if (parts[0] == "top" || parts[0] == "bottom") {
            ox = width * 0.5f;
            return parse_origin_component(parts[0], height, false, oy);
        }
        oy = height * 0.5f;
        return parse_origin_component(parts[0], width, true, ox);
    }
    // Two values: keywords may come in either order ("top left"); lengths are x then y.
    std::string first = parts[0];
    std::string second = parts[1];
    if (first == "top" || first == "bottom" || second == "left" || second == "right") {
        std::swap(first, second);
    }
    return parse_origin_component(first, width, true, ox) &&
           parse_origin_component(second, height, false, oy);
}

} // namespace

std::optional<Mat3> compute_transform(std::string_view transform, std::string_view origin,
                                      float width, float height) {
    const std::string_view text = trim(transform);
    if (text.empty() || lower(text) == "none") {
        return Mat3::identity();
    }

    Mat3 total = Mat3::identity();
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
            ++i;
        }
        if (i >= text.size()) {
            break;
        }
        const std::size_t name_begin = i;
        while (i < text.size() && text[i] != '(' &&
               std::isspace(static_cast<unsigned char>(text[i])) == 0) {
            ++i;
        }
        if (i >= text.size() || text[i] != '(') {
            return std::nullopt;
        }
        const std::string name = lower(text.substr(name_begin, i - name_begin));
        const std::size_t close = text.find(')', i);
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        const std::vector<std::string_view> args =
            split_arguments(text.substr(i + 1, close - i - 1));
        Mat3 next;
        if (!parse_function(name, args, width, height, next)) {
            return std::nullopt;
        }
        total = total * next;
        i = close + 1;
    }

    float ox = width * 0.5f;
    float oy = height * 0.5f;
    if (!resolve_origin(origin, width, height, ox, oy)) {
        ox = width * 0.5f;
        oy = height * 0.5f;
    }
    return Mat3::translation(ox, oy) * total * Mat3::translation(-ox, -oy);
}

} // namespace tessera::ui::style
