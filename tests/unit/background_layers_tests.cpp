// SPDX-License-Identifier: MIT
//
// Several background layers (issue #614): the `background-image` layer list, the `background`
// shorthand that produces it, the CSS bottom-to-top paint order, and the layer-wise interpolation.
//
// The list is CSS Backgrounds 3 section 2.1: a comma-separated list of images where the LAST layer
// is painted first and the FIRST layer ends up on top. V1 implements `linear-gradient()` layers and
// `none`; a `url()` layer is out of scope and is rejected with a diagnostic.

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/background_layers.hpp"
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
using tessera::ui::style::BackgroundLayerKind;
using tessera::ui::style::BackgroundLayerList;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Declaration;
using tessera::ui::style::format_background_layers;
using tessera::ui::style::interpolate_background_layers;
using tessera::ui::style::parse_background_layers;
using tessera::ui::style::PropertyRegistry;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "background_layers: " << message << "\n";
    }
    return condition;
}

bool near(float a, float b) {
    return std::fabs(a - b) < 0.001F;
}

// The interpolator prints through `format_color`, which quantizes a channel to 8 bits, so a value
// that went through a printed list is only accurate to 1/255 (0.004). Assertions on a re-parsed
// value use this instead of `near`.
bool near_byte(float a, float b) {
    return std::fabs(a - b) < 0.01F;
}

// The grammar: a comma-separated list of `linear-gradient()` and `none`, in CSS order.
bool grammar() {
    const auto two = parse_background_layers("linear-gradient(180deg, #ff0000 0%, #0000ff 100%), "
                                             "linear-gradient(90deg, #00ff00 0%, #000000 100%)");
    bool ok = expect(two && two->count == 2, "two gradient layers parse") &&
              expect(two && two->layers[0].kind == BackgroundLayerKind::LinearGradient &&
                         near(two->layers[0].gradient.angle_degrees, 180.0F),
                     "the first layer is the first value") &&
              expect(two && near(two->layers[1].gradient.angle_degrees, 90.0F),
                     "the second layer is the second value");

    const auto mixed = parse_background_layers("none, linear-gradient(red, blue)");
    ok = expect(mixed && mixed->count == 2 && mixed->layers[0].kind == BackgroundLayerKind::None &&
                    mixed->layers[1].kind == BackgroundLayerKind::LinearGradient,
                "`none` is a layer and keeps its slot") &&
         ok;

    const auto single = parse_background_layers("none");
    ok = expect(single && single->count == 1 && single->layers[0].kind == BackgroundLayerKind::None,
                "a lone `none` is one transparent layer") &&
         ok;

    // Out of grammar: a `url()` image is not in scope, and neither is a radial gradient (#613).
    ok = expect(!parse_background_layers("url(a.png), linear-gradient(red, blue)"),
                "a url() layer is rejected") &&
         ok;
    ok = expect(!parse_background_layers("radial-gradient(red, blue)"),
                "a radial gradient is rejected") &&
         ok;
    ok = expect(!parse_background_layers("linear-gradient(red, blue),"),
                "a trailing comma is rejected") &&
         ok;
    ok = expect(!parse_background_layers(""), "an empty value is rejected") && ok;
    ok = expect(!parse_background_layers("linear-gradient(red)"),
                "a one-stop gradient is rejected") &&
         ok;

    // The cap keeps the value fixed-size; 9 layers is past it.
    std::string nine;
    for (int index = 0; index < 9; ++index) {
        nine += index == 0 ? "none" : ", none";
    }
    ok = expect(!parse_background_layers(nine), "more than 8 layers is rejected") && ok;
    return ok;
}

// The printed form re-parses to the same list, so the interpolator and diagnostics can rely on it.
bool round_trip() {
    const auto list =
        parse_background_layers("linear-gradient(180deg, #ff0000 0%, #0000ff 100%), none, "
                                "linear-gradient(90deg, #00ff00 0%, #000000 100%)");
    if (!expect(list.has_value(), "the list parses")) {
        return false;
    }
    const std::string text = format_background_layers(*list);
    const auto again = parse_background_layers(text);
    bool ok = expect(again && again->count == list->count, "the printed list re-parses");
    if (again) {
        for (std::size_t index = 0; index < list->count; ++index) {
            ok = expect(again->layers[index].kind == list->layers[index].kind,
                        "a layer keeps its kind through a round trip") &&
                 ok;
            if (list->layers[index].kind == BackgroundLayerKind::LinearGradient) {
                ok = expect(near(again->layers[index].gradient.angle_degrees,
                                 list->layers[index].gradient.angle_degrees),
                            "a layer keeps its angle through a round trip") &&
                     ok;
            }
        }
    }
    return ok;
}

// The `background` shorthand produces the list, and only the final layer may carry the colour.
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

    const auto layered = expand("linear-gradient(red, blue), linear-gradient(green, black)");
    bool ok = expect(layered.diagnostics.empty(), "two layers expand without a diagnostic") &&
              expect(find(layered, "background-image") != nullptr &&
                         *find(layered, "background-image") ==
                             "linear-gradient(red, blue), linear-gradient(green, black)",
                     "the layers reach background-image in CSS order") &&
              expect(find(layered, "background-color") != nullptr &&
                         *find(layered, "background-color") == "transparent",
                     "the shorthand resets the colour it does not name");

    const auto with_color = expand("linear-gradient(red, blue), #102030");
    ok = expect(with_color.diagnostics.empty() && find(with_color, "background-color") != nullptr &&
                    *find(with_color, "background-color") == "#102030",
                "the colour on the final layer reaches background-color") &&
         ok;

    // CSS only allows the colour on the final layer; a colour on an earlier layer is a bad value.
    const auto early_color = expand("#102030, linear-gradient(red, blue)");
    ok = expect(early_color.declarations.empty() && !early_color.diagnostics.empty(),
                "a colour on a non-final layer rejects the declaration") &&
         ok;

    const auto url = expand("url(a.png), linear-gradient(red, blue)");
    ok = expect(url.declarations.empty() && !url.diagnostics.empty(),
                "a url() layer rejects the whole declaration, with a diagnostic") &&
         ok;

    // The longhand accepts the list too, and reports a value outside the grammar.
    const auto longhand = registry.expand(
        Declaration{"background-image", "none, linear-gradient(red, blue)", false, 0});
    ok = expect(longhand.declarations.size() == 1 && longhand.diagnostics.empty(),
                "background-image accepts a layer list") &&
         ok;
    const auto invalid = registry.expand(Declaration{"background-image", "url(a.png)", false, 0});
    ok = expect(!invalid.diagnostics.empty(), "an unsupported image value is reported") && ok;
    return ok;
}

// Interpolation: layer-wise, padded at the bottom, and a `none` fades rather than snaps.
bool interpolation() {
    const auto middle =
        interpolate_background_layers("linear-gradient(180deg, #ff0000 0%, #0000ff 100%)",
                                      "linear-gradient(180deg, #0000ff 0%, #ff0000 100%)", 0.5);
    bool ok = expect(middle.has_value(), "two gradients interpolate");
    if (middle) {
        const auto parsed = parse_background_layers(*middle);
        ok = expect(parsed && parsed->count == 1, "the result is one layer") &&
             expect(parsed && near_byte(parsed->layers[0].gradient.stops[0].color.r, 0.5F) &&
                        near_byte(parsed->layers[0].gradient.stops[0].color.b, 0.5F),
                    "the stops blend channel-wise") &&
             ok;
    }

    // A shorter list is padded at the bottom (the end of the list), so a layer added underneath the
    // existing ones fades in there instead of shifting the layers that were already painted.
    const auto padded = interpolate_background_layers(
        "linear-gradient(red, blue)", "linear-gradient(green, black), linear-gradient(red, blue)",
        0.0);
    ok = expect(padded.has_value(), "a list of a different length interpolates");
    if (padded) {
        const auto parsed = parse_background_layers(*padded);
        ok = expect(parsed && parsed->count == 2, "the result keeps the longer length") &&
             expect(parsed && parsed->layers[1].kind == BackgroundLayerKind::LinearGradient &&
                        near_byte(parsed->layers[1].gradient.stops[0].color.a, 0.0F),
                    "the padded bottom layer is transparent at t=0") &&
             expect(parsed && near_byte(parsed->layers[0].gradient.stops[0].color.g, 0.0F) &&
                        near_byte(parsed->layers[0].gradient.stops[0].color.r, 1.0F),
                    "the layer that was already there keeps its value at t=0") &&
             ok;
    }

    // `none` against a gradient is the transparent form of that gradient, so the fade is
    // continuous.
    const auto fade = interpolate_background_layers("none", "linear-gradient(red, blue)", 0.5);
    ok = expect(fade.has_value(), "`none` and a gradient interpolate");
    if (fade) {
        const auto parsed = parse_background_layers(*fade);
        ok = expect(parsed && parsed->count == 1 &&
                        parsed->layers[0].kind == BackgroundLayerKind::LinearGradient &&
                        near_byte(parsed->layers[0].gradient.stops[0].color.a, 0.5F),
                    "`none` fades the gradient's alpha instead of snapping") &&
             ok;
    }

    // The angle takes the shorter arc: 350deg to 10deg blends through 0, not through 180.
    const auto arc = interpolate_background_layers("linear-gradient(350deg, red, blue)",
                                                   "linear-gradient(10deg, red, blue)", 0.5);
    ok = expect(arc.has_value(), "two angles interpolate");
    if (arc) {
        const auto parsed = parse_background_layers(*arc);
        ok = expect(parsed && (near(parsed->layers[0].gradient.angle_degrees, 0.0F) ||
                               near(parsed->layers[0].gradient.angle_degrees, 360.0F)),
                    "the angle takes the shorter arc") &&
             ok;
    }

    // A value outside the grammar cannot interpolate, so the caller keeps its discrete switch.
    ok = expect(!interpolate_background_layers("url(a.png)", "none", 0.5),
                "an unsupported value does not interpolate") &&
         ok;
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

// The paint order is the CSS one: the last layer is emitted first, the first layer last (on top).
bool paint_order() {
    Node node(1);
    node.set_bounds({0, 0, 100, 50});
    StubStyles styles;
    ComputedStyle style;
    style.set("background-image", "linear-gradient(180deg, #ff0000 0%, #0000ff 100%), "
                                  "linear-gradient(90deg, #00ff00 0%, #000000 100%)");
    styles.set(1, style);

    RecordingSink sink;
    paint_scene(node, styles, sink);
    bool ok = expect(sink.gradients.size() == 2, "two layers paint two gradient fills");
    if (ok) {
        // The second value (90deg, green to black) is the bottom layer, so it is emitted first.
        ok = expect(near(sink.gradients[0].angle_degrees, 90.0F),
                    "the last layer is painted first (bottom)") &&
             expect(near(sink.gradients[1].angle_degrees, 180.0F),
                    "the first layer is painted last (on top)") &&
             expect(near(sink.gradients[0].stops[0].color.g, 1.0F),
                    "the bottom layer keeps its own stops") &&
             ok;
    }

    // `none` layers paint nothing but do not disturb the order of the layers around them.
    ComputedStyle with_none;
    with_none.set("background-image",
                  "none, linear-gradient(90deg, #00ff00 0%, #000000 100%), none");
    styles.set(1, with_none);
    RecordingSink quiet;
    paint_scene(node, styles, quiet);
    ok = expect(quiet.gradients.size() == 1 && near(quiet.gradients[0].angle_degrees, 90.0F),
                "`none` layers paint nothing and keep the order") &&
         ok;

    // A single layer keeps the pre-#614 output exactly.
    ComputedStyle single;
    single.set("background-image", "linear-gradient(90deg, #ff0000 0%, #0000ff 100%)");
    styles.set(1, single);
    RecordingSink one;
    paint_scene(node, styles, one);
    ok = expect(one.gradients.size() == 1 && near(one.gradients[0].angle_degrees, 90.0F),
                "a single layer paints one fill") &&
         ok;

    // An unsupported value paints nothing at all.
    ComputedStyle bad;
    bad.set("background-image", "url(a.png)");
    styles.set(1, bad);
    RecordingSink ignored;
    paint_scene(node, styles, ignored);
    ok = expect(ignored.gradients.empty(), "an unsupported value paints nothing") && ok;

    // Node opacity folds into every layer's stops, not just the first.
    ComputedStyle faded;
    faded.set("background-image", "linear-gradient(180deg, #ff0000 0%, #0000ff 100%), "
                                  "linear-gradient(90deg, #00ff00 0%, #000000 100%)");
    faded.set("opacity", "0.5");
    styles.set(1, faded);
    RecordingSink translucent;
    paint_scene(node, styles, translucent);
    ok = expect(translucent.gradients.size() == 2 &&
                    near(translucent.gradients[0].stops[0].color.a, 0.5F) &&
                    near(translucent.gradients[1].stops[0].color.a, 0.5F),
                "node opacity folds into every layer") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = grammar();
    ok = round_trip() && ok;
    ok = shorthand() && ok;
    ok = interpolation() && ok;
    ok = paint_order() && ok;
    if (!ok) {
        std::cerr << "background_layers: FAILED\n";
        return 1;
    }
    std::cout << "background_layers: ok\n";
    return 0;
}
