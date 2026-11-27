// SPDX-License-Identifier: MIT
//
// `background-image` gradients from a stylesheet (issues #457 and #613): the value grammar for
// linear, radial and the two repeating forms, the `background` shorthand, and the paint pass
// drawing the gradient from the resolved style.

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/gradient.hpp"
#include "tessera/ui/style/property_registry.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::GradientDraw;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::RadialGradientDraw;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Declaration;
using tessera::ui::style::GradientValue;
using tessera::ui::style::parse_gradient;
using tessera::ui::style::parse_linear_gradient;
using tessera::ui::style::PropertyRegistry;
using tessera::ui::style::RadialShape;
using tessera::ui::style::RadialSize;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "background_image: " << message << "\n";
    }
    return condition;
}

bool near(float a, float b) {
    return std::fabs(a - b) < 0.001F;
}

bool grammar() {
    const auto hero = parse_linear_gradient(
        "linear-gradient(135deg, #3b2bd9 0%, #8b3dff 55%, rgba(224, 67, 127, 0.5) 100%)");
    bool ok = expect(hero && hero->stop_count == 3 && near(hero->angle_degrees, 135.0F) &&
                         near(hero->stops[1].offset, 0.55F) && near(hero->stops[2].color.a, 0.5F),
                     "angle, three stops, positions and an rgba colour");

    const auto side = parse_linear_gradient("linear-gradient(to right, red, blue)");
    ok = expect(side && near(side->angle_degrees, 90.0F) && near(side->stops[0].offset, 0.0F) &&
                    near(side->stops[1].offset, 1.0F),
                "a side direction and implicit end positions") &&
         ok;

    const auto implicit = parse_linear_gradient("linear-gradient(red, yellow, blue, green)");
    ok = expect(implicit && near(implicit->angle_degrees, 180.0F) &&
                    near(implicit->stops[1].offset, 1.0F / 3.0F) &&
                    near(implicit->stops[2].offset, 2.0F / 3.0F),
                "default angle and evenly spread middle stops") &&
         ok;

    const auto turn = parse_linear_gradient("linear-gradient(0.25turn, red 80%, blue 20%)");
    ok = expect(turn && near(turn->angle_degrees, 90.0F) && near(turn->stops[1].offset, 0.8F),
                "turn unit; a stop behind an earlier one is raised to it") &&
         ok;

    ok = expect(!parse_linear_gradient("linear-gradient(red)"), "one stop is invalid") && ok;
    ok = expect(!parse_linear_gradient("linear-gradient(red, nonsense)"), "a bad colour") && ok;
    ok = expect(parse_linear_gradient("linear-gradient(red 10px, blue)").has_value(),
                "a length position is part of the grammar (gradient_stop_positions covers it)") &&
         ok;
    ok = expect(!parse_linear_gradient("linear-gradient(red 10em, blue)"),
                "a font-relative length has no font to resolve against") &&
         ok;
    ok = expect(!parse_linear_gradient("linear-gradient(to left right, red, blue)"),
                "contradictory direction") &&
         ok;
    return ok;
}

bool radial_grammar() {
    // The default shape is an ellipse at farthest-corner, centred.
    const auto plain = parse_gradient("radial-gradient(red, blue)");
    bool ok = expect(plain && plain->kind == GradientValue::Kind::Radial && !plain->repeating &&
                         plain->radial.shape == RadialShape::Ellipse &&
                         plain->radial.size == RadialSize::FarthestCorner &&
                         near(plain->radial.position.x, 0.5F) &&
                         near(plain->radial.position.y, 0.5F) && plain->radial.stop_count == 2,
                     "a bare radial gradient takes the CSS defaults");

    const auto circle = parse_gradient("radial-gradient(circle, red, blue)");
    ok =
        expect(circle && circle->radial.shape == RadialShape::Circle, "the circle shape keyword") &&
        ok;

    const auto sized = parse_gradient("radial-gradient(circle closest-side, red, blue)");
    ok = expect(sized && sized->radial.shape == RadialShape::Circle &&
                    sized->radial.size == RadialSize::ClosestSide,
                "a shape and a size keyword") &&
         ok;

    const auto corner =
        parse_gradient("radial-gradient(ellipse farthest-corner at 25% 75%, red, blue)");
    ok =
        expect(corner && corner->radial.size == RadialSize::FarthestCorner &&
                   near(corner->radial.position.x, 0.25F) && near(corner->radial.position.y, 0.75F),
               "a size keyword and an `at` position") &&
        ok;

    const auto keywords = parse_gradient("radial-gradient(at left top, red, blue)");
    ok = expect(keywords && near(keywords->radial.position.x, 0.0F) &&
                    near(keywords->radial.position.y, 0.0F),
                "position keywords resolve to fractions") &&
         ok;

    const auto explicit_radii = parse_gradient("radial-gradient(40px 20px at 50% 50%, red, blue)");
    ok = expect(explicit_radii && explicit_radii->radial.size == RadialSize::Explicit &&
                    near(explicit_radii->radial.radius_x, 40.0F) &&
                    near(explicit_radii->radial.radius_y, 20.0F),
                "explicit radii") &&
         ok;

    const auto one_radius = parse_gradient("radial-gradient(circle 30px, red, blue)");
    ok = expect(one_radius && one_radius->radial.size == RadialSize::Explicit &&
                    near(one_radius->radial.radius_x, 30.0F) &&
                    near(one_radius->radial.radius_y, 30.0F),
                "one explicit radius is a circle") &&
         ok;

    const auto eight = parse_gradient("radial-gradient(red, orange, yellow, green, blue, indigo, "
                                      "violet, black)");
    ok = expect(eight && eight->radial.stop_count == 8, "eight stops are accepted") && ok;

    ok = expect(!parse_gradient("radial-gradient(red)"), "a radial gradient needs two stops") && ok;
    ok = expect(!parse_gradient("radial-gradient(cone, red, blue)"),
                "an unknown shape keyword is rejected") &&
         ok;
    ok = expect(!parse_gradient("radial-gradient(at 10px 20px, red, blue)"),
                "a px position needs the box, so it is outside the grammar") &&
         ok;
    ok = expect(!parse_gradient("radial-gradient(red, nonsense)"), "a bad radial colour") && ok;
    return ok;
}

bool repeating_grammar() {
    const auto repeating_linear = parse_gradient("repeating-linear-gradient(red 0%, blue 10%)");
    bool ok = expect(repeating_linear && repeating_linear->repeating &&
                         repeating_linear->kind == GradientValue::Kind::Linear &&
                         repeating_linear->linear.stop_count == 2,
                     "the repeating linear form parses");

    const auto repeating_radial =
        parse_gradient("repeating-radial-gradient(circle, red, blue 20%)");
    ok = expect(repeating_radial && repeating_radial->repeating &&
                    repeating_radial->kind == GradientValue::Kind::Radial,
                "the repeating radial form parses") &&
         ok;

    // The expansion repeats the authored cycle until it spans 0..1. The destination must hold
    // kMaxRepeatingStops entries, which is the documented contract.
    std::array<tessera::ui::style::GradientStopValue, tessera::ui::style::kMaxRepeatingStops>
        stops{};
    for (std::size_t index = 0; index < repeating_linear->linear.stop_count; ++index) {
        stops[index] = repeating_linear->linear.stops[index];
    }
    const std::size_t expanded = tessera::ui::style::expand_repeating_stops(
        stops.data(), repeating_linear->linear.stop_count);
    ok = expect(expanded > 2, "a repeating ramp expands past its authored stops") && ok;
    ok = expect(near(stops[0].offset, 0.0F) && near(stops[expanded - 1].offset, 1.0F),
                "the expanded ramp spans the whole gradient line") &&
         ok;
    // Offsets accumulate in float, so a repeated cycle can land a hair below its predecessor; the
    // ramp only has to be non-decreasing within a tolerance.
    bool monotonic = true;
    for (std::size_t index = 1; index < expanded; ++index) {
        if (stops[index].offset < stops[index - 1].offset - 1.0e-5F) {
            monotonic = false;
        }
    }
    ok = expect(monotonic, "the expanded ramp stays sorted") && ok;

    // A ramp that already spans 0..1 is its own single cycle.
    std::array<tessera::ui::style::GradientStopValue, tessera::ui::style::kMaxRepeatingStops>
        full{};
    full[0] = repeating_linear->linear.stops[0];
    full[1] = repeating_linear->linear.stops[1];
    full[0].offset = 0.0F;
    full[1].offset = 1.0F;
    ok = expect(tessera::ui::style::expand_repeating_stops(full.data(), 2) == 2,
                "a full-span ramp is not repeated") &&
         ok;

    ok = expect(!parse_gradient("repeating-linear-gradient(red)"),
                "a repeating gradient still needs two stops") &&
         ok;
    return ok;
}

bool shorthand() {
    const PropertyRegistry registry;
    const auto expand = [&](const char* value) {
        return registry.expand(Declaration{"background", value, false, 0});
    };
    const auto find = [](const auto& result, const char* property) -> const std::string* {
        for (const Declaration& declaration : result.declarations) {
            if (declaration.property == property) {
                return &declaration.value;
            }
        }
        return nullptr;
    };

    const auto both = expand("#102030 linear-gradient(to bottom, red, blue)");
    bool ok = expect(find(both, "background-color") != nullptr &&
                         find(both, "background-image") != nullptr && both.diagnostics.empty(),
                     "a colour and a gradient expand to both longhands");
    const auto only_gradient = expand("linear-gradient(red, blue)");
    ok = expect(find(only_gradient, "background-color") != nullptr &&
                    *find(only_gradient, "background-color") == "transparent",
                "the shorthand resets the longhand it does not name") &&
         ok;
    const auto radial = expand("radial-gradient(circle, red, blue)");
    ok = expect(find(radial, "background-image") != nullptr && radial.diagnostics.empty(),
                "the shorthand accepts a radial gradient") &&
         ok;
    const auto repeating = expand("repeating-linear-gradient(red 0%, blue 10%)");
    ok = expect(find(repeating, "background-image") != nullptr && repeating.diagnostics.empty(),
                "the shorthand accepts a repeating gradient") &&
         ok;
    const auto url = expand("url(a.png) red");
    ok = expect(url.declarations.empty() && !url.diagnostics.empty(),
                "an unsupported layer rejects the whole declaration, with a diagnostic") &&
         ok;
    const auto longhand = registry.expand(Declaration{"background-image", "none", false, 0});
    ok = expect(longhand.declarations.size() == 1, "background-image is a known property") && ok;
    // `conic-gradient()` is supported since #617, so the unsupported case is a `url()` layer.
    const auto invalid = registry.expand(Declaration{"background-image", "url(a.png)", false, 0});
    ok = expect(!invalid.diagnostics.empty(), "an unsupported image value is reported") && ok;
    const auto conic =
        registry.expand(Declaration{"background-image", "conic-gradient(red, blue)", false, 0});
    ok = expect(conic.diagnostics.empty(), "a conic gradient is accepted") && ok;
    const auto accepted =
        registry.expand(Declaration{"background-image", "radial-gradient(red, blue)", false, 0});
    ok = expect(accepted.diagnostics.empty(), "a radial gradient is accepted") && ok;
    return ok;
}

class StubStyles final : public StyleProvider {
public:
    void set(NodeId id, ComputedStyle style) {
        styles_[id] = std::move(style);
    }
    ComputedStyle compute(const Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? ComputedStyle{} : found->second;
    }

private:
    std::map<NodeId, ComputedStyle> styles_;
};

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const tessera::rendering::rhi::Rect&,
                   const tessera::rendering::rhi::Color&) override {
        ++fills;
    }
    void fill_rounded_rect(const tessera::rendering::rhi::Rect&,
                           const tessera::rendering::rhi::Color&, float) override {
        ++fills;
    }
    void fill_linear_gradient(const GradientDraw& draw) override {
        gradients.push_back(draw);
    }
    void fill_radial_gradient(const RadialGradientDraw& draw) override {
        radials.push_back(draw);
    }
    int fills = 0;
    std::vector<GradientDraw> gradients;
    std::vector<RadialGradientDraw> radials;
};

bool paints_from_style() {
    Node node(1);
    node.set_bounds({0, 0, 100, 50});
    StubStyles styles;
    ComputedStyle style;
    style.set("background-color", "#112233");
    style.set("background-image", "linear-gradient(90deg, #ff0000 0%, #0000ff 100%)");
    style.set("opacity", "0.5");
    styles.set(1, style);

    RecordingSink sink;
    paint_scene(node, styles, sink);
    bool ok = expect(sink.gradients.size() == 1, "the stylesheet value paints one gradient");
    if (ok) {
        const GradientDraw& draw = sink.gradients.front();
        ok = expect(draw.stop_count == 2 && near(draw.angle_degrees, 90.0F) &&
                        near(draw.stops[0].color.r, 1.0F) && near(draw.stops[1].color.b, 1.0F),
                    "angle and stops reach the sink") &&
             expect(near(draw.stops[0].color.a, 0.5F), "node opacity is folded into the stops");
    }
    ok = expect(sink.fills >= 1, "the solid background still paints underneath") && ok;

    ComputedStyle none;
    none.set("background-image", "none");
    styles.set(1, none);
    RecordingSink quiet;
    paint_scene(node, styles, quiet);
    ok = expect(quiet.gradients.empty() && quiet.radials.empty(),
                "background-image: none paints no gradient") &&
         ok;

    ComputedStyle bad;
    bad.set("background-image", "conic-gradient(red, blue)");
    styles.set(1, bad);
    RecordingSink ignored;
    paint_scene(node, styles, ignored);
    return expect(ignored.gradients.empty() && ignored.radials.empty(),
                  "an unsupported value paints nothing") &&
           ok;
}

bool paints_radial_from_style() {
    Node node(1);
    node.set_bounds({0, 0, 100, 50});
    StubStyles styles;
    ComputedStyle style;
    style.set("background-image",
              "radial-gradient(circle closest-side at 25% 50%, #ff0000, #0000ff)");
    style.set("opacity", "0.5");
    styles.set(1, style);

    RecordingSink sink;
    paint_scene(node, styles, sink);
    bool ok = expect(sink.radials.size() == 1 && sink.gradients.empty(),
                     "a radial value paints one radial gradient and no linear one");
    if (ok) {
        const RadialGradientDraw& draw = sink.radials.front();
        // Centre at 25 % of 100 px, 50 % of 50 px; closest-side is the nearer of the two x/y
        // distances, so 25 px horizontally and 25 px vertically.
        ok = expect(near(draw.center_x, 25.0F) && near(draw.center_y, 25.0F),
                    "the centre resolves against the node box") &&
             expect(near(draw.radius_x, 25.0F) && near(draw.radius_y, 25.0F),
                    "closest-side resolves against the node box") &&
             expect(draw.stop_count == 2 && near(draw.stops[0].color.a, 0.5F),
                    "stops reach the sink with node opacity folded in");
    }

    // A repeating gradient reaches the sink as authored, with `repeating` set: the renderer wraps
    // the ramp, so no period can overflow a stop cap.
    ComputedStyle repeating;
    repeating.set("background-image", "repeating-radial-gradient(circle, #ff0000 0%, #0000ff 10%)");
    styles.set(1, repeating);
    RecordingSink wrapped;
    paint_scene(node, styles, wrapped);
    ok = expect(wrapped.radials.size() == 1 && wrapped.radials.front().stop_count == 2 &&
                    wrapped.radials.front().repeating &&
                    near(wrapped.radials.front().stops[1].offset, 0.1F),
                "a repeating radial gradient arrives unexpanded with its period") &&
         ok;
    return ok;
}

} // namespace

// `conic-gradient()` (issue #617): the prelude is `[from <angle>]? [at <position>]?`, both
// optional, and the stop list is the one the other two shapes share.
bool conic_grammar() {
    const auto plain = parse_gradient("conic-gradient(red, blue)");
    bool ok = expect(plain && plain->kind == GradientValue::Kind::Conic && !plain->repeating &&
                         near(plain->conic.from_angle_degrees, 0.0F) &&
                         near(plain->conic.position.x, 0.5F) &&
                         near(plain->conic.position.y, 0.5F) && plain->conic.stop_count == 2,
                     "a bare conic gradient takes the CSS defaults");

    const auto from = parse_gradient("conic-gradient(from 90deg, red, blue)");
    ok = expect(from && near(from->conic.from_angle_degrees, 90.0F), "the `from` angle is read") &&
         ok;

    const auto turn = parse_gradient("conic-gradient(from 0.25turn, red, blue)");
    ok = expect(turn && near(turn->conic.from_angle_degrees, 90.0F), "`turn` is a unit") && ok;

    const auto at = parse_gradient("conic-gradient(at 25% 75%, red, blue)");
    ok = expect(at && near(at->conic.position.x, 0.25F) && near(at->conic.position.y, 0.75F),
                "the `at` position is read") &&
         ok;

    const auto both = parse_gradient("conic-gradient(from 45deg at 10% 20%, red, blue)");
    ok = expect(both && near(both->conic.from_angle_degrees, 45.0F) &&
                    near(both->conic.position.x, 0.1F) && near(both->conic.position.y, 0.2F),
                "`from` and `at` combine") &&
         ok;

    const auto repeating = parse_gradient("repeating-conic-gradient(red, blue)");
    ok = expect(repeating && repeating->kind == GradientValue::Kind::Conic && repeating->repeating,
                "the repeating form is recognised") &&
         ok;

    // Out of grammar: a stop list of one, and a prelude token that is not `from`/`at`.
    ok = expect(!parse_gradient("conic-gradient(red)"), "a one-stop conic gradient is rejected") &&
         ok;
    ok = expect(!parse_gradient("conic-gradient(to right, red, blue)"),
                "a linear direction is not a conic prelude") &&
         ok;
    return ok;
}
int main() {
    bool ok = grammar();
    ok = radial_grammar() && ok;
    ok = conic_grammar() && ok;
    ok = repeating_grammar() && ok;
    ok = shorthand() && ok;
    ok = paints_from_style() && ok;
    ok = paints_radial_from_style() && ok;
    return ok ? 0 : 1;
}
