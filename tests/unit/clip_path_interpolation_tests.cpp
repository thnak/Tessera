// SPDX-License-Identifier: MIT
//
// `clip-path` shape interpolation (ADR-068 slice 6, issue #674): same-kind shapes blend component
// by component, everything else stays discrete, and every interpolated value is one the paint-time
// parser reads. The keyframe test samples at fixed steps, so it needs no clock.

#include "../../src/ui/animation/clip_path_interpolation.hpp"
#include "tessera/ui/animation/keyframes.hpp"
#include "tessera/ui/animation/property_dirty.hpp"
#include "tessera/ui/paint/clip_path.hpp"
#include "tessera/ui/style/keyframes.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

namespace animation = tessera::ui::animation;
namespace detail = tessera::ui::paint::detail;
namespace style = tessera::ui::style;
using animation::interpolate_clip_path;

int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "clip_path_interpolation: " << message << "\n";
        ++g_failures;
    }
}

void expect_text(const std::optional<std::string>& actual, const std::string& expected,
                 const std::string& message) {
    expect(actual.has_value() && *actual == expected,
           message + " (got " + (actual ? "\"" + *actual + "\"" : std::string("nothing")) + ")");
}

void test_inset() {
    expect_text(interpolate_clip_path("inset(0px)", "inset(20px)", 0.5),
                "inset(10px 10px 10px 10px)", "an inset blends its four edges");
    expect_text(interpolate_clip_path("inset(0px 10px)", "inset(20px 30px 40px 50px)", 0.5),
                "inset(10px 20px 20px 30px)", "the 1-to-4 expansion applies before blending");
    // A side without `round` is square corners, so the radii blend from zero.
    expect_text(interpolate_clip_path("inset(10px)", "inset(10px round 8px)", 0.5),
                "inset(10px 10px 10px 10px round 4px 4px 4px 4px)",
                "a missing round clause blends from square corners");
    expect_text(
        interpolate_clip_path("inset(0px round 4px 8px)", "inset(0px round 12px 16px)", 0.25),
        "inset(0px 0px 0px 0px round 6px 10px 6px 10px)", "each corner blends");
    expect_text(interpolate_clip_path("inset(10%)", "inset(30%)", 0.5), "inset(20% 20% 20% 20%)",
                "percentages stay percentages");
}

void test_circle_and_ellipse() {
    expect_text(interpolate_clip_path("circle(10px at 0px 0px)", "circle(30px at 100px 50px)", 0.5),
                "circle(20px at 50px 25px)", "a circle blends its radius and centre");
    expect_text(interpolate_clip_path("circle(0%)", "circle(50%)", 0.5), "circle(25% at 50% 50%)",
                "the default centre is 50% 50%");
    expect_text(
        interpolate_clip_path("circle(10px at left top)", "circle(30px at right bottom)", 0.5),
        "circle(20px at 50% 50%)", "position keywords are percentages");
    expect_text(interpolate_clip_path("ellipse(10px 20px)", "ellipse(30px 60px at 0% 100%)", 0.5),
                "ellipse(20px 40px at 25% 75%)", "an ellipse blends both radii and the centre");
}

void test_polygon() {
    expect_text(interpolate_clip_path("polygon(0 0, 100px 0, 100px 100px)",
                                      "polygon(0 0, 200px 0, 100px 200px)", 0.5),
                "polygon(0px 0px, 150px 0px, 100px 150px)", "polygon vertices blend pairwise");
    expect_text(interpolate_clip_path("polygon(evenodd, 0 0, 10px 0, 5px 8px)",
                                      "polygon(evenodd, 0 0, 30px 0, 5px 8px)", 1.0),
                "polygon(evenodd, 0px 0px, 30px 0px, 5px 8px)", "the fill rule is kept");
    expect(!interpolate_clip_path("polygon(0 0, 10px 0, 5px 8px)",
                                  "polygon(0 0, 10px 0, 10px 8px, 5px 8px)", 0.5),
           "polygons with different vertex counts do not interpolate");
    expect(!interpolate_clip_path("polygon(evenodd, 0 0, 10px 0, 5px 8px)",
                                  "polygon(nonzero, 0 0, 10px 0, 5px 8px)", 0.5),
           "polygons with different fill rules do not interpolate");
}

void test_discrete() {
    expect(!interpolate_clip_path("none", "circle(10px)", 0.5), "none against a shape is discrete");
    expect(!interpolate_clip_path("circle(10px)", "none", 0.5), "a shape against none is discrete");
    expect(!interpolate_clip_path("circle(10px)", "ellipse(10px 10px)", 0.5),
           "two different functions are discrete");
    expect(!interpolate_clip_path("inset(10px)", "circle(10px)", 0.5),
           "inset against circle is discrete");
    expect(!interpolate_clip_path("circle(10px)", "circle(50%)", 0.5),
           "px against % is discrete (CSS would need calc())");
    expect(!interpolate_clip_path("url(#a)", "url(#b)", 0.5), "url() is discrete");
    expect(!interpolate_clip_path("path('M0 0')", "path('M1 1')", 0.5), "path() is discrete");
    expect(!interpolate_clip_path("circle(calc(10px + 1%))", "circle(30px)", 0.5),
           "a nested function is not interpolated");
    expect(!interpolate_clip_path("inset(", "inset(10px)", 0.5), "a malformed shape is discrete");
    // Zero has no unit, so it blends with either.
    expect_text(interpolate_clip_path("circle(0)", "circle(40%)", 0.5), "circle(20% at 50% 50%)",
                "a zero length takes the other side's unit");
}

void test_ends_and_clamp() {
    const std::string a = "inset(4px 8px 12px 16px round 2px 4px 6px 8px)";
    const std::string b = "inset(10px 20px 30px 40px round 10px 12px 14px 16px)";
    expect_text(interpolate_clip_path(a, b, 0.0), a, "t = 0 is the from shape");
    expect_text(interpolate_clip_path(a, b, 1.0), b, "t = 1 is the to shape");
    // A radius cannot go negative when the progress overshoots (a back-easing curve).
    expect_text(interpolate_clip_path("circle(10px)", "circle(20px)", -2.0),
                "circle(0px at 50% 50%)", "an overshoot clamps a radius at zero");
    // An edge inset can go negative: it grows the shape past the box.
    expect_text(interpolate_clip_path("inset(10px)", "inset(20px)", -1.0), "inset(0px 0px 0px 0px)",
                "an inset extrapolates");
}

void test_paint_parser_reads_the_result() {
    const tessera::rendering::rhi::Rect box{0.0f, 0.0f, 200.0f, 100.0f};
    const std::vector<std::pair<std::string, std::string>> pairs = {
        {"inset(0px)", "inset(20% round 8px)"},
        {"circle(10% at left top)", "circle(40% at 75% 25%)"},
        {"ellipse(10% 20%)", "ellipse(40% 10% at 10% 90%)"},
        {"polygon(0 0, 100% 0, 50% 100%)", "polygon(10% 10%, 90% 10%, 50% 60%)"},
        {"polygon(evenodd, 0 0, 100% 0, 50% 100%)", "polygon(evenodd, 10% 10%, 90% 10%, 50% 60%)"},
    };
    for (const auto& [from, to] : pairs) {
        for (const double t : {0.0, 0.25, 0.5, 0.75, 1.0}) {
            const auto mid = interpolate_clip_path(from, to, t);
            if (!mid) {
                expect(false, "interpolates: " + from);
                continue;
            }
            const auto parsed = detail::parse_clip_path(*mid, box);
            expect(!parsed.invalid && !parsed.unsupported && !parsed.shape.is_none(),
                   "the paint parser reads \"" + *mid + "\"");
        }
    }
    // The paint-time shapes of the two ends bracket the midpoint: a circle centre 10px -> 30px.
    const auto start = detail::parse_clip_path(
        *interpolate_clip_path("circle(10px at 0px 0px)", "circle(30px at 100px 0px)", 0.0), box);
    const auto end = detail::parse_clip_path(
        *interpolate_clip_path("circle(10px at 0px 0px)", "circle(30px at 100px 0px)", 1.0), box);
    expect(start.shape.radius_x == 10.0f && start.shape.center_x == 0.0f,
           "t = 0 resolves to the from circle");
    expect(end.shape.radius_x == 30.0f && end.shape.center_x == 100.0f,
           "t = 1 resolves to the to circle");
}

// The same blend through the keyframe sampler, at fixed steps (no clock).
void test_keyframes() {
    style::KeyframeBlock block;
    block.name = "morph";
    block.steps.push_back(style::KeyframeStep{
        0.0,
        {style::Declaration{"clip-path", "polygon(0 0, 100px 0, 100px 100px, 0 100px)", false,
                            0}}});
    block.steps.push_back(style::KeyframeStep{
        100.0,
        {style::Declaration{"clip-path", "polygon(50px 0, 100px 50px, 50px 100px, 0 50px)", false,
                            0}}});
    animation::KeyframeTiming timing;
    timing.duration = 1.0;
    timing.fill = animation::FillMode::Both;

    const auto at = [&](double seconds) {
        const auto sampled = animation::sample_keyframes(block, timing, seconds);
        return sampled.empty() ? std::string() : sampled.front().value;
    };
    // Frame 0 is the authored from value untouched; every later frame is the sampled blend, so the
    // last one is the to shape in the interpolator's normalised spelling.
    expect(at(0.0) == "polygon(0 0, 100px 0, 100px 100px, 0 100px)", "frame 0 is the from shape");
    expect(at(0.5) == "polygon(25px 0px, 100px 25px, 75px 100px, 0px 75px)",
           "the midpoint morphs every vertex");
    expect(at(1.0) == "polygon(50px 0px, 100px 50px, 50px 100px, 0px 50px)",
           "frame 1 is the to shape");

    // A kind change inside the same animation switches discretely at the halfway point.
    block.steps[1].declarations[0].value = "circle(40px)";
    expect(at(0.25) == "polygon(0 0, 100px 0, 100px 100px, 0 100px)",
           "before halfway the from shape holds");
    expect(at(0.75) == "circle(40px)", "after halfway the to shape holds");
}

void test_subtree_dirty() {
    expect(animation::paint_dirties_subtree("clip-path"),
           "clip-path clips every descendant primitive, so it dirties the subtree like opacity");
    expect(!animation::paint_dirties_subtree("background-color"), "a colour does not");
}

} // namespace

int main() {
    test_inset();
    test_circle_and_ellipse();
    test_polygon();
    test_discrete();
    test_ends_and_clamp();
    test_paint_parser_reads_the_result();
    test_keyframes();
    test_subtree_dirty();
    if (g_failures != 0) {
        std::cerr << "clip_path_interpolation: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "clip_path_interpolation: ok\n";
    return 0;
}
