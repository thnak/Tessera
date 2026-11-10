// SPDX-License-Identifier: MIT
//
// CSS transform / transform-origin resolution: function semantics (left-to-right composition,
// origin handling, percentages, angle units) and rejection of invalid values.

#include "tessera/ui/style/transform.hpp"

#include <cmath>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

using tessera::rendering::rhi::Mat3;
using tessera::ui::style::compute_transform;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "transform: " << message << "\n";
    }
    return condition;
}

struct Point {
    float x = 0.0f;
    float y = 0.0f;
};

Point apply(const Mat3& matrix, float x, float y) {
    Point out;
    matrix.transform_point(x, y, out.x, out.y);
    return out;
}

bool near(const Point& point, float x, float y) {
    return std::abs(point.x - x) < 0.01f && std::abs(point.y - y) < 0.01f;
}

bool none_and_invalid() {
    const auto none = compute_transform("none", "50% 50%", 100, 100);
    bool ok = expect(none.has_value() && near(apply(*none, 7, 9), 7, 9), "none is the identity");
    ok = expect(!compute_transform("translate(10)", "", 100, 100).has_value(),
                "a unitless non-zero length is invalid") &&
         ok;
    ok =
        expect(!compute_transform("foo(1)", "", 100, 100).has_value(), "an unknown function") && ok;
    ok = expect(!compute_transform("rotate(10)", "", 100, 100).has_value(),
                "a unitless non-zero angle is invalid") &&
         ok;
    ok = expect(!compute_transform("scale(2", "", 100, 100).has_value(), "an unclosed function") &&
         ok;
    ok = expect(!compute_transform("translate(1px, 2px, 3px)", "", 100, 100).has_value(),
                "too many arguments") &&
         ok;
    return ok;
}

bool translate_and_percent() {
    const auto absolute = compute_transform("translate(10px, 20px)", "", 200, 100);
    const auto percent = compute_transform("translate(50%, 25%)", "", 200, 100);
    const auto axis = compute_transform("translateX(5px) translateY(-3px)", "", 200, 100);
    return expect(absolute.has_value() && near(apply(*absolute, 0, 0), 10, 20),
                  "translate in px") &&
           expect(percent.has_value() && near(apply(*percent, 0, 0), 100, 25),
                  "translate percentages use the box width/height") &&
           expect(axis.has_value() && near(apply(*axis, 0, 0), 5, -3), "translateX/translateY");
}

bool scale_around_origin() {
    const auto centered = compute_transform("scale(2)", "", 100, 50);
    const auto corner = compute_transform("scale(2)", "left top", 100, 50);
    const auto anisotropic = compute_transform("scale(2, 3)", "0 0", 100, 50);
    return expect(centered.has_value() && near(apply(*centered, 50, 25), 50, 25) &&
                      near(apply(*centered, 0, 0), -50, -25),
                  "the default origin is the box center") &&
           expect(corner.has_value() && near(apply(*corner, 10, 10), 20, 20) &&
                      near(apply(*corner, 0, 0), 0, 0),
                  "origin keywords left top anchor the scale at the corner") &&
           expect(anisotropic.has_value() && near(apply(*anisotropic, 1, 1), 2, 3),
                  "scale(x, y) with a px origin");
}

bool rotate_and_skew() {
    const auto quarter = compute_transform("rotate(90deg)", "", 100, 100);
    const auto turn = compute_transform("rotate(0.25turn)", "", 100, 100);
    const auto rad = compute_transform("rotate(1.5707964rad)", "", 100, 100);
    const auto grad = compute_transform("rotate(100grad)", "", 100, 100);
    const auto skew = compute_transform("skewX(45deg)", "0 0", 100, 100);
    // A clockwise quarter turn about the center sends the top-left corner to the top-right.
    return expect(quarter.has_value() && near(apply(*quarter, 0, 0), 100, 0),
                  "rotate(90deg) turns the box clockwise about its center") &&
           expect(turn.has_value() && near(apply(*turn, 0, 0), 100, 0), "turn unit") &&
           expect(rad.has_value() && near(apply(*rad, 0, 0), 100, 0), "rad unit") &&
           expect(grad.has_value() && near(apply(*grad, 0, 0), 100, 0), "grad unit") &&
           expect(skew.has_value() && near(apply(*skew, 0, 10), 10, 10), "skewX shifts x by y");
}

bool composition_is_left_to_right() {
    // CSS applies the rightmost function to the point first: scale, then translate.
    const auto both = compute_transform("translate(10px, 0) scale(2)", "0 0", 100, 100);
    const auto matrix = compute_transform("matrix(1, 0, 0, 1, 5, 7)", "0 0", 100, 100);
    return expect(both.has_value() && near(apply(*both, 1, 1), 12, 2),
                  "functions compose left to right (rightmost applies first)") &&
           expect(matrix.has_value() && near(apply(*matrix, 0, 0), 5, 7), "matrix(a..f)");
}

} // namespace

int main() {
    bool ok = true;
    ok = none_and_invalid() && ok;
    ok = translate_and_percent() && ok;
    ok = scale_around_origin() && ok;
    ok = rotate_and_skew() && ok;
    ok = composition_is_left_to_right() && ok;
    if (!ok) {
        std::cerr << "transform: FAILED\n";
        return 1;
    }
    std::cout << "transform: ok\n";
    return 0;
}
