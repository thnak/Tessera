// SPDX-License-Identifier: MIT
//
// `background-image` gradient interpolation (issue #613): two gradients of the same shape blend
// stop by stop and blend their geometry, a shape or repeating mismatch stays discrete, and a value
// that is not a gradient is left to the caller's 50 % switch.

#include "../../src/ui/animation/gradient_interpolation.hpp"
#include "tessera/ui/style/gradient.hpp"

#include <cmath>
#include <iostream>
#include <string>

namespace {

using tessera::ui::animation::interpolate_gradient;
using tessera::ui::style::parse_gradient;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "gradient_interpolation: " << message << "\n";
    }
    return condition;
}

bool linear_blends() {
    const auto mid = interpolate_gradient("linear-gradient(0deg, #000000, #ffffff)",
                                          "linear-gradient(180deg, #ffffff, #000000)", 0.5);
    bool ok = expect(mid.has_value(), "two linear gradients interpolate");
    if (ok) {
        const auto parsed = parse_gradient(*mid);
        ok = expect(parsed && parsed->kind == tessera::ui::style::GradientValue::Kind::Linear,
                    "the result is still a linear gradient") &&
             expect(parsed && std::abs(parsed->linear.angle_degrees - 90.0f) < 0.01f,
                    "the angle blends") &&
             expect(parsed && parsed->linear.stop_count == 2, "the stop count is preserved");
    }
    // The stop colours blend too: black -> white and white -> black both land on mid grey.
    if (ok) {
        const auto parsed = parse_gradient(*mid);
        ok = expect(parsed && std::abs(parsed->linear.stops[0].color.r - 0.5f) < 0.01f &&
                        std::abs(parsed->linear.stops[1].color.r - 0.5f) < 0.01f,
                    "the stop colours blend");
    }
    return ok;
}

bool stop_count_mismatch() {
    const auto mid =
        interpolate_gradient("linear-gradient(red, blue)", "linear-gradient(red, lime, blue)", 0.5);
    bool ok = expect(mid.has_value(), "a stop-count mismatch still interpolates");
    if (ok) {
        const auto parsed = parse_gradient(*mid);
        ok = expect(parsed && parsed->linear.stop_count == 3,
                    "the longer stop list sets the result length");
    }
    return ok;
}

bool radial_blends() {
    const auto mid =
        interpolate_gradient("radial-gradient(circle 10px at 0% 0%, #000000, #ffffff)",
                             "radial-gradient(circle 30px at 100% 100%, #000000, #ffffff)", 0.5);
    bool ok = expect(mid.has_value(), "two radial gradients interpolate");
    if (ok) {
        const auto parsed = parse_gradient(*mid);
        ok = expect(parsed && parsed->kind == tessera::ui::style::GradientValue::Kind::Radial,
                    "the result is still a radial gradient") &&
             expect(parsed && std::abs(parsed->radial.radius_x - 20.0f) < 0.01f,
                    "the radius blends") &&
             expect(parsed && std::abs(parsed->radial.position.x - 0.5f) < 0.01f &&
                        std::abs(parsed->radial.position.y - 0.5f) < 0.01f,
                    "the centre blends");
    }
    return ok;
}

bool discrete_cases() {
    bool ok = expect(
        !interpolate_gradient("linear-gradient(red, blue)", "radial-gradient(red, blue)", 0.5)
             .has_value(),
        "a shape change is discrete");
    ok = expect(!interpolate_gradient("linear-gradient(red, blue)",
                                      "repeating-linear-gradient(red, blue)", 0.5)
                     .has_value(),
                "a repeating mismatch is discrete") &&
         ok;
    ok = expect(!interpolate_gradient("radial-gradient(circle, red, blue)",
                                      "radial-gradient(ellipse, red, blue)", 0.5)
                     .has_value(),
                "a circle/ellipse change is discrete") &&
         ok;
    ok = expect(!interpolate_gradient("none", "linear-gradient(red, blue)", 0.5).has_value(),
                "a non-gradient value is left to the caller") &&
         ok;
    ok = expect(!interpolate_gradient("linear-gradient(red, blue)", "linear-gradient(red, blue)",
                                      std::nan(""))
                     .has_value(),
                "a non-finite progress is rejected") &&
         ok;
    return ok;
}

bool endpoints() {
    const auto at_zero = interpolate_gradient("linear-gradient(0deg, #000000, #ffffff)",
                                              "linear-gradient(180deg, #000000, #ffffff)", 0.0);
    const auto at_one = interpolate_gradient("linear-gradient(0deg, #000000, #ffffff)",
                                             "linear-gradient(180deg, #000000, #ffffff)", 1.0);
    bool ok = expect(at_zero.has_value() && at_one.has_value(), "the endpoints interpolate");
    if (ok) {
        const auto zero = parse_gradient(*at_zero);
        const auto one = parse_gradient(*at_one);
        ok = expect(zero && std::abs(zero->linear.angle_degrees) < 0.01f,
                    "t = 0 is the from value") &&
             expect(one && std::abs(one->linear.angle_degrees - 180.0f) < 0.01f,
                    "t = 1 is the to value");
    }
    return ok;
}

} // namespace

int main() {
    bool ok = linear_blends();
    ok = stop_count_mismatch() && ok;
    ok = radial_blends() && ok;
    ok = discrete_cases() && ok;
    ok = endpoints() && ok;
    if (!ok) {
        std::cerr << "gradient_interpolation: failed\n";
        return 1;
    }
    std::cout << "gradient_interpolation: ok\n";
    return 0;
}
