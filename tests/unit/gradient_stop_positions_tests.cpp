// SPDX-License-Identifier: MIT
//
// Gradient colour-stop positions (issue #617, CSS Images 4 section 3.5): a stop with two positions,
// length positions (px and the other absolute units) resolved against the gradient line, ray or
// box, angles on a conic stop, positions outside 0..1, and what paint hands the renderer for a
// repeating ramp. Chromium measurements of the same values live in
// tests/gpu/batched2d/gradient_chromium_gpu_tests.cpp.

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/gradient.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::ConicGradientDraw;
using tessera::ui::paint::GradientDraw;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::RadialGradientDraw;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::GradientStopValue;
using tessera::ui::style::GradientValue;
using tessera::ui::style::parse_gradient;
using tessera::ui::style::resolve_stop_positions;
using tessera::ui::style::StopPositions;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "gradient_stop_positions: " << message << "\n";
    }
    return condition;
}

bool near(float a, float b) {
    return std::fabs(a - b) < 0.0005F;
}

// A stop list's offsets as one comparable string, so a failure shows the whole ramp.
template<typename Stops> std::string offsets(const Stops& stops, std::size_t count) {
    std::string text;
    for (std::size_t index = 0; index < count; ++index) {
        text += std::to_string(stops[index].offset) + " ";
    }
    return text;
}

bool two_positions() {
    const auto value = parse_gradient("linear-gradient(red 0 50%, blue 50% 100%)");
    bool ok = expect(value && value->linear.stop_count == 4 && !value->linear.positions.pending(),
                     "a stop with two positions is two stops, all fractions");
    if (ok) {
        const auto& stops = value->linear.stops;
        ok = expect(near(stops[0].offset, 0.0F) && near(stops[1].offset, 0.5F) &&
                        near(stops[2].offset, 0.5F) && near(stops[3].offset, 1.0F),
                    "the two positions are the two offsets") &&
             expect(near(stops[0].color.r, 1.0F) && near(stops[1].color.r, 1.0F) &&
                        near(stops[2].color.b, 1.0F) && near(stops[3].color.b, 1.0F),
                    "both stops of a pair carry the colour");
    }

    // A colour with spaces inside its parentheses, and a lone single position, still read.
    const auto spaced =
        parse_gradient("linear-gradient(rgb(255 0 0) 10% 30%, rgb(0 0 255 / 50%) 90%)");
    ok = expect(spaced && spaced->linear.stop_count == 3 &&
                    near(spaced->linear.stops[1].offset, 0.3F) &&
                    near(spaced->linear.stops[2].color.a, 0.5F),
                "a space-separated colour function takes one or two positions") &&
         ok;

    // Eight stops is the cap, counted after a pair is split.
    ok = expect(
             parse_gradient("linear-gradient(red 0 10%, blue 10% 20%, red 20% 30%, blue 30% 100%)")
                 .has_value(),
             "four pairs make the eight-stop cap") &&
         ok;
    ok = expect(
             !parse_gradient("linear-gradient(red 0 10%, blue 10% 20%, red 20% 30%, blue 30% 40%, "
                             "red 40%)")
                  .has_value(),
             "a ninth stop after splitting pairs is out of the grammar") &&
         ok;
    // A third position, a colour hint and a relative unit are outside the grammar, never guessed.
    ok = expect(!parse_gradient("linear-gradient(red 0 10% 20%, blue)"),
                "three positions are rejected") &&
         ok;
    ok = expect(!parse_gradient("linear-gradient(red, 30%, blue)"), "a colour hint is rejected") &&
         ok;
    ok = expect(!parse_gradient("linear-gradient(red 2em, blue)"), "em has no font to resolve") &&
         ok;
    ok = expect(!parse_gradient("linear-gradient(red 5, blue)"),
                "a unitless number is no position") &&
         ok;
    return ok;
}

bool length_positions() {
    const auto stripes = parse_gradient("repeating-linear-gradient(red 0 10px, blue 10px 20px)");
    bool ok = expect(stripes && stripes->repeating && stripes->linear.stop_count == 4 &&
                         stripes->linear.positions.pending(),
                     "px positions wait for the box");
    if (ok) {
        const auto& stops = stripes->linear.stops;
        // The bare 0 is a fraction (the same place in either unit); the rest are px as authored.
        ok = expect(stripes->linear.positions.length_mask == 0b1110 &&
                        stripes->linear.positions.auto_mask == 0,
                    "the three px stops are marked") &&
             expect(near(stops[1].offset, 10.0F) && near(stops[2].offset, 10.0F) &&
                        near(stops[3].offset, 20.0F),
                    "px offsets are kept as authored");
    }

    const auto open = parse_gradient("repeating-linear-gradient(red, blue 20px)");
    ok = expect(open && open->linear.positions.length_mask == 0b10 &&
                    open->linear.positions.auto_mask == 0b01,
                "a missing first position waits beside a px one") &&
         ok;

    // Other absolute units convert at 96 dpi.
    const auto inches = parse_gradient("linear-gradient(red 0.5in, blue 2.54cm)");
    ok = expect(inches && near(inches->linear.stops[0].offset, 48.0F) &&
                    near(inches->linear.stops[1].offset, 96.0F),
                "in and cm convert to px") &&
         ok;

    // Radial positions are px along the ray; the prelude is still told from a first stop.
    const auto radial =
        parse_gradient("repeating-radial-gradient(circle, red 0 5px, blue 5px 10px)");
    ok = expect(radial && radial->kind == GradientValue::Kind::Radial &&
                    radial->radial.stop_count == 4 && radial->radial.positions.pending(),
                "radial px stops parse after a prelude") &&
         ok;
    const auto bare = parse_gradient("radial-gradient(red 20%, blue)");
    ok = expect(bare && bare->radial.stop_count == 2 && near(bare->radial.stops[0].offset, 0.2F),
                "a radial gradient with no prelude and a positioned first stop parses") &&
         ok;
    return ok;
}

bool conic_positions() {
    const auto angles =
        parse_gradient("conic-gradient(red 0deg 90deg, blue 90deg 180deg, lime 0.75turn)");
    bool ok = expect(angles && angles->conic.stop_count == 5 && !angles->conic.positions.pending(),
                     "conic angles are turn fractions, never pending");
    if (ok) {
        const auto& stops = angles->conic.stops;
        ok = expect(near(stops[0].offset, 0.0F) && near(stops[1].offset, 0.25F) &&
                        near(stops[2].offset, 0.25F) && near(stops[3].offset, 0.5F) &&
                        near(stops[4].offset, 0.75F),
                    "0deg 90deg 90deg 180deg 0.75turn") &&
             expect(near(stops[3].color.b, 1.0F), "the pair shares its colour");
    }
    const auto percent = parse_gradient("conic-gradient(red 10%, blue 60%)");
    ok = expect(percent && near(percent->conic.stops[0].offset, 0.1F), "percent still works") && ok;
    ok = expect(!parse_gradient("conic-gradient(red 10px, blue)"),
                "a length is not a conic position") &&
         ok;
    return ok;
}

bool out_of_range_positions() {
    const auto wide = parse_gradient("linear-gradient(red -20%, blue 120%)");
    bool ok = expect(wide && near(wide->linear.stops[0].offset, -0.2F) &&
                         near(wide->linear.stops[1].offset, 1.2F),
                     "positions outside 0..1 are kept, not clamped");
    // A position behind an earlier one is raised to it (the fix-up of CSS Images 3 section 3.4.3).
    const auto behind = parse_gradient("linear-gradient(red 60%, blue 20%, lime)");
    ok = expect(behind && near(behind->linear.stops[1].offset, 0.6F) &&
                    near(behind->linear.stops[2].offset, 1.0F),
                "a stop behind an earlier one is raised to it") &&
         ok;
    return ok;
}

bool resolves_against_the_line() {
    // 0, 10px, 10px, 20px on a 40px line: 0, .25, .25, .5.
    auto value = parse_gradient("repeating-linear-gradient(red 0 10px, blue 10px 20px)");
    bool ok = expect(value.has_value(), "stripes parse");
    if (!ok) {
        return false;
    }
    auto& linear = value->linear;
    resolve_stop_positions(linear.stops.data(), linear.stop_count, linear.positions, 40.0F);
    ok = expect(!linear.positions.pending(), "resolving clears the pending marks") &&
         expect(near(linear.stops[0].offset, 0.0F) && near(linear.stops[1].offset, 0.25F) &&
                    near(linear.stops[2].offset, 0.25F) && near(linear.stops[3].offset, 0.5F),
                offsets(linear.stops, 4).c_str());

    // A missing first stop is 0 and a missing last one is 100 % of the line, not the px beside it.
    auto open = parse_gradient("linear-gradient(red, lime 20px, blue)");
    ok = expect(open.has_value(), "open list parses") && ok;
    if (open) {
        auto& stops = open->linear;
        resolve_stop_positions(stops.stops.data(), stops.stop_count, stops.positions, 80.0F);
        ok = expect(near(stops.stops[0].offset, 0.0F) && near(stops.stops[1].offset, 0.25F) &&
                        near(stops.stops[2].offset, 1.0F),
                    "first 0, px / line, last 100 %") &&
             ok;
    }

    // A middle stop with no position sits halfway between its resolved neighbours, in fractions.
    auto spread = parse_gradient("linear-gradient(red 10px, lime, blue 70%)");
    ok = expect(spread.has_value(), "spread list parses") && ok;
    if (spread) {
        auto& stops = spread->linear;
        resolve_stop_positions(stops.stops.data(), stops.stop_count, stops.positions, 100.0F);
        ok = expect(near(stops.stops[1].offset, 0.4F), "a missing middle stop is spread evenly") &&
             ok;
    }

    // A px stop behind an earlier percentage is raised to it.
    auto behind = parse_gradient("linear-gradient(red 60%, blue 10px)");
    ok = expect(behind.has_value(), "mixed units parse") && ok;
    if (behind) {
        auto& stops = behind->linear;
        resolve_stop_positions(stops.stops.data(), stops.stop_count, stops.positions, 100.0F);
        ok = expect(near(stops.stops[1].offset, 0.6F),
                    "10px (10 %) behind 60 % is raised to 60 %") &&
             ok;
    }

    // A degenerate line cannot divide by zero, and a resolved list is left alone.
    auto degenerate = parse_gradient("linear-gradient(red 5px, blue 9px)");
    if (degenerate) {
        auto& stops = degenerate->linear;
        resolve_stop_positions(stops.stops.data(), stops.stop_count, stops.positions, 0.0F);
        ok = expect(std::isfinite(stops.stops[1].offset) && near(stops.stops[1].offset, 9.0F),
                    "a zero-length line resolves as one px") &&
             ok;
        const float before = stops.stops[1].offset;
        resolve_stop_positions(stops.stops.data(), stops.stop_count, stops.positions, 50.0F);
        ok = expect(stops.stops[1].offset == before, "resolving twice is a no-op") && ok;
    }
    return ok;
}

// --- through the paint pass -------------------------------------------------------------------

class Styles final : public StyleProvider {
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

class Recorder final : public PaintSink {
public:
    void fill_rect(const tessera::rendering::rhi::Rect&,
                   const tessera::rendering::rhi::Color&) override {}
    void fill_rounded_rect(const tessera::rendering::rhi::Rect&,
                           const tessera::rendering::rhi::Color&, float) override {}
    void fill_linear_gradient(const GradientDraw& draw) override {
        linear.push_back(draw);
    }
    void fill_radial_gradient(const RadialGradientDraw& draw) override {
        radial.push_back(draw);
    }
    void fill_conic_gradient(const ConicGradientDraw& draw) override {
        conic.push_back(draw);
    }
    std::vector<GradientDraw> linear;
    std::vector<RadialGradientDraw> radial;
    std::vector<ConicGradientDraw> conic;
};

Recorder paint(const char* image, float width, float height) {
    Node node(1);
    node.set_bounds({0, 0, width, height});
    Styles styles;
    ComputedStyle style;
    style.set("background-image", image);
    styles.set(1, style);
    Recorder recorder;
    paint_scene(node, styles, recorder);
    return recorder;
}

bool paints_resolved_positions() {
    // 0 / 10px / 10px / 20px along a vertical 200px line (180deg, the default angle): the period
    // is 20px = 0.1 of the line, and the renderer gets the four authored stops, not 40 expanded.
    const Recorder stripes =
        paint("repeating-linear-gradient(red 0 10px, blue 10px 20px)", 100.0F, 200.0F);
    bool ok = expect(stripes.linear.size() == 1, "one linear draw");
    if (ok) {
        const GradientDraw& draw = stripes.linear.front();
        ok = expect(draw.repeating && draw.stop_count == 4, "repeating, not expanded") &&
             expect(near(draw.stops[1].offset, 0.05F) && near(draw.stops[2].offset, 0.05F) &&
                        near(draw.stops[3].offset, 0.1F),
                    offsets(draw.stops, 4).c_str());
    }

    // The same text on a different box: the line is 100px wide at 90deg, so 10px is 0.1.
    const Recorder wide = paint("linear-gradient(90deg, red 10px, blue 90px)", 100.0F, 20.0F);
    ok = expect(wide.linear.size() == 1 && near(wide.linear.front().stops[0].offset, 0.1F) &&
                    near(wide.linear.front().stops[1].offset, 0.9F),
                "px resolves against the line of the box being painted") &&
         ok;

    // An oblique line: 45deg across 100x100 is 141.42px long.
    const Recorder oblique = paint("linear-gradient(45deg, red 0, blue 100px)", 100.0F, 100.0F);
    ok = expect(oblique.linear.size() == 1 &&
                    near(oblique.linear.front().stops[1].offset, 100.0F / 141.42136F),
                "an oblique line is |w sin| + |h cos| long") &&
         ok;

    // A radial ray is the horizontal radius: closest-side at the centre of a 80x40 box is 40.
    const Recorder radial = paint(
        "repeating-radial-gradient(ellipse closest-side, red 0 10px, blue 20px)", 80.0F, 40.0F);
    ok = expect(radial.radial.size() == 1 && radial.radial.front().repeating &&
                    near(radial.radial.front().stops[1].offset, 10.0F / 40.0F) &&
                    near(radial.radial.front().stops[2].offset, 20.0F / 40.0F),
                "radial px is measured along the horizontal radius") &&
         ok;

    // Out-of-range fractions reach the renderer as they are.
    const Recorder beyond = paint("linear-gradient(red -20%, blue 120%)", 100.0F, 100.0F);
    ok = expect(beyond.linear.size() == 1 && near(beyond.linear.front().stops[0].offset, -0.2F) &&
                    near(beyond.linear.front().stops[1].offset, 1.2F),
                "positions are not clamped to 0..1") &&
         ok;

    // A conic angle is a turn fraction at paint too.
    const Recorder sweep = paint("repeating-conic-gradient(red 0deg, blue 45deg)", 100.0F, 100.0F);
    ok = expect(sweep.conic.size() == 1 && sweep.conic.front().repeating &&
                    near(sweep.conic.front().stops[1].offset, 0.125F),
                "conic angles reach the renderer as turn fractions") &&
         ok;
    return ok;
}

bool degenerate_repeat_is_one_colour() {
    // A repeating ramp whose first and last stop coincide has no period to repeat. CSS Images 3
    // says the average colour; Chromium paints the last stop's colour over the whole box
    // (measured, gradient_chromium_reference_stops.hpp), and the engine follows Chromium.
    const Recorder flat = paint(
        "repeating-linear-gradient(#ff0000 10px, #00ff00 10px, #0000ff 10px)", 100.0F, 100.0F);
    bool ok = expect(flat.linear.size() == 1 && flat.linear.front().stop_count == 1 &&
                         !flat.linear.front().repeating,
                     "a zero period collapses to one stop");
    if (ok) {
        const auto& color = flat.linear.front().stops[0].color;
        ok = expect(near(color.r, 0.0F) && near(color.g, 0.0F) && near(color.b, 1.0F) &&
                        near(color.a, 1.0F),
                    "the one stop is the last colour");
    }
    return ok;
}

} // namespace

int main() {
    bool ok = two_positions();
    ok = length_positions() && ok;
    ok = conic_positions() && ok;
    ok = out_of_range_positions() && ok;
    ok = resolves_against_the_line() && ok;
    ok = paints_resolved_positions() && ok;
    ok = degenerate_repeat_is_one_colour() && ok;
    if (ok) {
        std::cout << "gradient_stop_positions: ok\n";
    }
    return ok ? 0 : 1;
}
