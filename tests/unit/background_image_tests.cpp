// SPDX-License-Identifier: MIT
//
// `background-image: linear-gradient()` from a stylesheet (issue #457): the value grammar, the
// `background` shorthand, and the paint pass drawing the gradient from the resolved style.

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/gradient.hpp"
#include "tessera/ui/style/property_registry.hpp"

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
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Declaration;
using tessera::ui::style::parse_linear_gradient;
using tessera::ui::style::PropertyRegistry;

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
    ok = expect(!parse_linear_gradient("radial-gradient(red, blue)"), "radial is not supported") &&
         ok;
    ok = expect(!parse_linear_gradient("repeating-linear-gradient(red, blue)"),
                "repeating is not supported") &&
         ok;
    ok = expect(!parse_linear_gradient("linear-gradient(red, nonsense)"), "a bad colour") && ok;
    ok = expect(!parse_linear_gradient("linear-gradient(red 10px, blue)"),
                "a length position is outside the grammar") &&
         ok;
    ok = expect(!parse_linear_gradient("linear-gradient(to left right, red, blue)"),
                "contradictory direction") &&
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
    const auto url = expand("url(a.png) red");
    ok = expect(url.declarations.empty() && !url.diagnostics.empty(),
                "an unsupported layer rejects the whole declaration, with a diagnostic") &&
         ok;
    const auto longhand = registry.expand(Declaration{"background-image", "none", false, 0});
    ok = expect(longhand.declarations.size() == 1, "background-image is a known property") && ok;
    const auto invalid =
        registry.expand(Declaration{"background-image", "radial-gradient(red, blue)", false, 0});
    ok = expect(!invalid.diagnostics.empty(), "an unsupported image value is reported") && ok;
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
    int fills = 0;
    std::vector<GradientDraw> gradients;
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
    ok = expect(quiet.gradients.empty(), "background-image: none paints no gradient") && ok;

    ComputedStyle bad;
    bad.set("background-image", "radial-gradient(red, blue)");
    styles.set(1, bad);
    RecordingSink ignored;
    paint_scene(node, styles, ignored);
    return expect(ignored.gradients.empty(), "an unsupported value paints nothing") && ok;
}

} // namespace

int main() {
    bool ok = grammar();
    ok = shorthand() && ok;
    ok = paints_from_style() && ok;
    return ok ? 0 : 1;
}
