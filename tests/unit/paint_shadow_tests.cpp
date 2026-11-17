// SPDX-License-Identifier: MIT
//
// CSS box-shadow: parsing (lengths, colors, lists, invalid values), emission order (beneath the
// node's own background, first listed on top), spread, offset, opacity, the fact that an outer
// shadow is never painted inside the node's border box (#461), the blurred layer stack's Gaussian
// edge profile, the one-shape box shadow a shader sink gets, and the outsets damage uses.

#include "paint_shadow.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::detail::parse_box_shadows;
using tessera::ui::paint::detail::shadow_outsets;
using tessera::ui::style::ComputedStyle;

// The node every case paints: {10, 10, 50, 40}.
constexpr Rect kNode{10, 10, 50, 40};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_shadow: " << message << "\n";
    }
    return condition;
}

ComputedStyle styled(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle style;
    for (const auto& [property, value] : values) {
        style.set(property, value);
    }
    return style;
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

struct Op {
    Rect rect;
    Color color;
};

// A flattened sink: no one-shape box shadow, so every layer arrives as the strips around the border
// box that the paint pass emits.
class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color& color) override {
        ops.push_back({rect, color});
    }
    void fill_rounded_rect(const Rect& rect, const Color& color, float) override {
        ops.push_back({rect, color});
    }
    std::vector<Op> ops;
};

struct Painted {
    std::vector<Op> ops;
    PaintStats stats;
};

Painted paint(ComputedStyle style, bool reduce_effects = false) {
    Node node(1);
    node.set_bounds(kNode);
    StubStyles styles;
    styles.set(1, std::move(style));
    RecordingSink sink;
    tessera::ui::paint::PaintContext context;
    context.reduce_effects = reduce_effects;
    Painted result;
    result.stats = paint_scene(node, styles, sink, context);
    result.ops = std::move(sink.ops);
    return result;
}

bool near(float a, float b) {
    return std::abs(a - b) < 0.01f;
}

bool same_rect(const Rect& rect, float x, float y, float width, float height) {
    return near(rect.x, x) && near(rect.y, y) && near(rect.width, width) &&
           near(rect.height, height);
}

// True when `rect` shares no area with the node's border box: the outer-shadow clipping rule.
bool outside(const Rect& rect) {
    const float left = std::max(rect.x, kNode.x);
    const float top = std::max(rect.y, kNode.y);
    const float right = std::min(rect.x + rect.width, kNode.x + kNode.width);
    const float bottom = std::min(rect.y + rect.height, kNode.y + kNode.height);
    return left >= right || top >= bottom;
}

bool sharp_shadow_is_carved() {
    // 4 px right and 6 px down: only the band below the node and the band right of it survive.
    const Painted offset = paint(styled({{"box-shadow", "4px 6px 0 0 #000000"}}));
    bool ok = expect(offset.ops.size() == 2 && offset.stats.shadows == 1,
                     "a sharp offset shadow is two strips and one primitive");
    if (ok) {
        const bool below = same_rect(offset.ops[0].rect, 14, 50, 50, 6);
        const bool right = same_rect(offset.ops[1].rect, 60, 16, 4, 34);
        ok = expect(below && right, "the strips are the node box minus the border box") && ok;
    }
    ok = expect(std::all_of(offset.ops.begin(), offset.ops.end(),
                            [](const Op& op) { return outside(op.rect); }),
                "no strip touches the border box") &&
         ok;

    // A spread grows the box on every side and yields all four strips.
    const Painted spread = paint(styled({{"box-shadow", "0 0 0 5px #ff0000"}}));
    ok = expect(spread.ops.size() == 4, "a concentric spread paints four strips") && ok;
    if (spread.ops.size() == 4) {
        ok = expect(same_rect(spread.ops[0].rect, 5, 5, 60, 5) &&
                        same_rect(spread.ops[1].rect, 5, 50, 60, 5) &&
                        same_rect(spread.ops[2].rect, 5, 10, 5, 40) &&
                        same_rect(spread.ops[3].rect, 60, 10, 5, 40),
                    "the strips wrap the border box") &&
             ok;
    }
    ok = expect(near(spread.ops.empty() ? 0.0f : spread.ops[0].color.a, 1.0f),
                "the shadow keeps the color alpha") &&
         ok;

    // The background paints over the shadow, and the shadow never covers the node's own box.
    const Painted with_background =
        paint(styled({{"box-shadow", "2px 2px #000000"}, {"background-color", "#ffffff"}}));
    ok = expect(with_background.ops.size() == 3 && with_background.ops.back().color.r > 0.9f,
                "the shadow is painted beneath the background") &&
         ok;
    return ok;
}

bool background_is_carved_too() {
    // A blurred shadow entirely under a transparent box paints nothing inside it, and every strip
    // it does paint stays outside.
    const Painted blurred = paint(styled({{"box-shadow", "0 0 12px #000000"}}));
    bool ok = expect(!blurred.ops.empty(), "the blurred shadow still paints its visible bands");
    ok = expect(std::all_of(blurred.ops.begin(), blurred.ops.end(),
                            [](const Op& op) { return outside(op.rect); }),
                "a blurred shadow never paints inside the border box") &&
         ok;
    return ok;
}

bool opacity_and_lists() {
    const Painted faded = paint(styled({{"box-shadow", "1px 1px #000000"}, {"opacity", "0.5"}}));
    const Painted two =
        paint(styled({{"box-shadow", "1px 1px #ff0000, 2px 2px rgba(0, 0, 255, 1)"}}));
    return expect(!faded.ops.empty() && near(faded.ops[0].color.a, 0.5f),
                  "node opacity multiplies the shadow alpha") &&
           expect(two.ops.size() == 4 && two.ops.back().color.r > 0.9f &&
                      two.ops.front().color.b > 0.9f,
                  "the first listed shadow paints on top (emitted last)");
}

bool invalid_none_and_inset() {
    const Painted none = paint(styled({{"box-shadow", "none"}}));
    const Painted missing = paint(ComputedStyle{});
    const Painted too_short = paint(styled({{"box-shadow", "4px"}}));
    const Painted bad_color = paint(styled({{"box-shadow", "1px 2px notacolor"}}));
    const Painted negative_blur = paint(styled({{"box-shadow", "1px 2px -3px #000"}}));
    const Painted partly_bad = paint(styled({{"box-shadow", "1px 1px #000, nonsense"}}));
    const Painted inset = paint(styled({{"box-shadow", "inset 0 0 5px #000"}}));
    const Painted unitless = paint(styled({{"box-shadow", "3 4 #000"}}));
    return expect(none.ops.empty() && missing.ops.empty(), "none and a missing property") &&
           expect(too_short.ops.empty(), "fewer than two lengths is invalid") &&
           expect(bad_color.ops.empty(), "an unparseable color is invalid") &&
           expect(negative_blur.ops.empty(), "a negative blur is invalid") &&
           expect(partly_bad.ops.empty(), "one invalid entry invalidates the list") &&
           expect(inset.ops.empty(), "inset shadows are parsed but not painted yet") &&
           expect(unitless.ops.empty(), "unit-less non-zero lengths are invalid");
}

bool blur_follows_a_gaussian_edge() {
    const Painted blurred = paint(styled({{"box-shadow", "0 0 12px #000000"}}));
    // Six layers of four strips; the layers that fit inside the border box are carved away
    // entirely.
    bool ok = expect(blurred.stats.shadows == 6, "a blurred shadow is a stack of six layers");
    ok = expect(!blurred.ops.empty() && blurred.ops.size() % 4 == 0,
                "each visible layer emits the four strips") &&
         ok;
    // The outermost group reaches one blur radius out on every side.
    ok = expect(same_rect(blurred.ops[0].rect, 10 - 12, 10 - 12, 50 + 24, 12),
                "the outermost layer reaches one blur radius out") &&
         ok;
    // The outermost band is the faint tail of the Gaussian, and the composite rises toward the box.
    const float tail = blurred.ops[0].color.a;
    ok = expect(tail > 0.0f && tail < 0.2f, "the outer band is a faint tail") && ok;
    float cumulative = 0.0f;
    bool rising = true;
    for (std::size_t i = 0; i < blurred.ops.size(); i += 4) {
        const float next = 1.0f - (1.0f - cumulative) * (1.0f - blurred.ops[i].color.a);
        rising = rising && next >= cumulative;
        cumulative = next;
    }
    ok = expect(rising, "coverage never decreases toward the interior") && ok;
    // The band that touches the border box carries roughly half the shadow, as the Gaussian edge
    // does at the box: 0.5 * (1 - erf(0)) = 0.5, minus the layer quantisation.
    ok = expect(cumulative > 0.25f && cumulative < 0.6f,
                "the innermost visible band is about half the shadow alpha") &&
         ok;
    return ok;
}

bool reduced_effects_keep_the_reach_with_fewer_layers() {
    const Painted full = paint(styled({{"box-shadow", "0 0 12px #000000"}}));
    const Painted reduced = paint(styled({{"box-shadow", "0 0 12px #000000"}}), true);
    bool ok = expect(reduced.stats.shadows == 2 && reduced.stats.shadows < full.stats.shadows,
                     "reduced effects emit the capped number of blur layers");
    // The outermost layer is the shadow's full reach, so damage and the visible silhouette hold.
    ok = expect(!reduced.ops.empty() && !full.ops.empty() &&
                    same_rect(reduced.ops[0].rect, full.ops[0].rect.x, full.ops[0].rect.y,
                              full.ops[0].rect.width, full.ops[0].rect.height),
                "the outer extent of a reduced shadow equals the full one") &&
         ok;
    // A sharp shadow is one layer already: reduction changes nothing.
    const Painted sharp_full = paint(styled({{"box-shadow", "4px 4px 0 #000000"}}));
    const Painted sharp_reduced = paint(styled({{"box-shadow", "4px 4px 0 #000000"}}), true);
    ok = expect(sharp_full.stats.shadows == 1 && sharp_reduced.stats.shadows == 1,
                "a sharp shadow is unaffected by reduction") &&
         ok;
    // The sync flag the controller's stage probe reads: only a blurred outer shadow counts.
    ok = expect(shadow_outsets(styled({{"box-shadow", "0 0 12px #000000"}})).blurred,
                "a blurred shadow is flagged") &&
         ok;
    ok = expect(!shadow_outsets(styled({{"box-shadow", "4px 4px 0 #000000"}})).blurred &&
                    !shadow_outsets(styled({{"box-shadow", "none"}})).blurred &&
                    !shadow_outsets(styled({{"box-shadow", "inset 0 0 8px #000000"}})).blurred,
                "sharp, absent and inset shadows are not flagged") &&
         ok;
    return ok;
}

bool outsets_reach() {
    const auto outsets = shadow_outsets(styled({{"box-shadow", "10px 5px 4px 2px #000"}}));
    const auto none = shadow_outsets(styled({{"box-shadow", "none"}}));
    const auto inset = shadow_outsets(styled({{"box-shadow", "inset 3px 3px 4px #000"}}));
    // reach = spread 2 + blur 4 = 6: left 6-10 -> 0, right 6+10, top 6-5, bottom 6+5.
    return expect(near(outsets.left, 0) && near(outsets.right, 16) && near(outsets.top, 1) &&
                      near(outsets.bottom, 11),
                  "outsets account for offset, spread and blur") &&
           expect(near(none.left, 0) && near(none.right, 0), "none has no outsets") &&
           expect(near(inset.left, 0) && near(inset.right, 0) && near(inset.bottom, 0),
                  "inset shadows do not extend outside the box");
}

bool list_is_bounded() {
    const auto list = parse_box_shadows(
        styled({{"box-shadow", "1px 1px red, 2px 2px red, 3px 3px red, 4px 4px red, 5px 5px red"}}),
        Color{0, 0, 0, 1});
    return expect(list.count == tessera::ui::paint::detail::kMaxBoxShadows,
                  "the shadow list is capped");
}

} // namespace

// A sink that can draw a whole shadow shape gets one call per shadow instead of the strips: the
// shadow box (offset and spread applied), the node's border box to carve out, the colour with the
// node's alpha, the corner radius grown by the spread, and sigma = blur / 2 (0 for a sharp shadow).
class OneShapeSink final : public PaintSink {
public:
    void fill_rect(const Rect&, const Color&) override {
        ++fills;
    }
    void fill_rounded_rect(const Rect&, const Color&, float) override {
        ++fills;
    }
    [[nodiscard]] bool supports_box_shadow() const override {
        return true;
    }
    void fill_box_shadow(const tessera::ui::paint::BoxShadowDraw& draw) override {
        draws.push_back(draw);
    }
    std::vector<tessera::ui::paint::BoxShadowDraw> draws;
    int fills = 0;
};

bool one_shape_shadow_is_one_call() {
    Node node(1);
    node.set_bounds(kNode);
    StubStyles styles;
    styles.set(1, styled({{"box-shadow", "3px 4px 12px 2px rgba(0,0,0,0.5)"},
                          {"border-top-left-radius", "6px"},
                          {"border-top-right-radius", "6px"},
                          {"border-bottom-right-radius", "6px"},
                          {"border-bottom-left-radius", "6px"}}));
    OneShapeSink sink;
    const PaintStats stats = paint_scene(node, styles, sink);
    bool ok = expect(sink.draws.size() == 1 && sink.fills == 0 && stats.shadows == 1,
                     "one box-shadow call and no strips");
    if (!ok) {
        return false;
    }
    const auto& draw = sink.draws[0];
    ok = expect(same_rect(draw.box, 11, 12, 54, 44),
                "the box is the node box moved by the offset and grown by the spread") &&
         ok;
    ok = expect(same_rect(draw.carve.rect, kNode.x, kNode.y, kNode.width, kNode.height) &&
                    near(draw.carve.top_left, 6.0f) && near(draw.carve.bottom_right, 6.0f),
                "the carve box is the node's rounded border box") &&
         ok;
    ok = expect(near(draw.sigma, 6.0f) && near(draw.radius, 8.0f),
                "sigma is half the blur and the radius grows with the spread") &&
         ok;
    ok = expect(near(draw.color.a, 0.5f), "the colour keeps the shadow alpha") && ok;

    // A sharp shadow goes through the same call with sigma 0, so it is carved too.
    styles.set(1, styled({{"box-shadow", "0 0 0 2px #000000"},
                          {"border-top-left-radius", "6px"},
                          {"border-top-right-radius", "6px"},
                          {"border-bottom-right-radius", "6px"},
                          {"border-bottom-left-radius", "6px"}}));
    OneShapeSink sharp;
    paint_scene(node, styles, sharp);
    ok = expect(sharp.draws.size() == 1 && near(sharp.draws[0].sigma, 0.0f) &&
                    near(sharp.draws[0].radius, 8.0f) && near(sharp.draws[0].carve.top_left, 6.0f),
                "a sharp shadow is one call with sigma 0, carved and rounded") &&
         ok;
    return ok;
}

int main() {
    bool ok = true;
    ok = one_shape_shadow_is_one_call() && ok;
    ok = sharp_shadow_is_carved() && ok;
    ok = background_is_carved_too() && ok;
    ok = opacity_and_lists() && ok;
    ok = invalid_none_and_inset() && ok;
    ok = blur_follows_a_gaussian_edge() && ok;
    ok = reduced_effects_keep_the_reach_with_fewer_layers() && ok;
    ok = outsets_reach() && ok;
    ok = list_is_bounded() && ok;
    if (!ok) {
        std::cerr << "paint_shadow: FAILED\n";
        return 1;
    }
    std::cout << "paint_shadow: ok\n";
    return 0;
}
