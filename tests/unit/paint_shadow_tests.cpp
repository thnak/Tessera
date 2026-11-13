// SPDX-License-Identifier: MIT
//
// CSS box-shadow: parsing (lengths, colors, lists, invalid values), emission order (beneath the
// node's own background, first listed on top), spread, offset, radius, opacity, the blurred layer
// stack's Gaussian edge profile, and the outsets damage uses.

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
    float radius = 0.0f;
};

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color& color) override {
        ops.push_back({rect, color, 0.0f});
    }
    void fill_rounded_rect(const Rect& rect, const Color& color, float radius) override {
        ops.push_back({rect, color, radius});
    }
    std::vector<Op> ops;
};

struct Painted {
    std::vector<Op> ops;
    PaintStats stats;
};

Painted paint(ComputedStyle style) {
    Node node(1);
    node.set_bounds({10, 10, 50, 40});
    StubStyles styles;
    styles.set(1, std::move(style));
    RecordingSink sink;
    Painted result;
    result.stats = paint_scene(node, styles, sink);
    result.ops = std::move(sink.ops);
    return result;
}

bool near(float a, float b) {
    return std::abs(a - b) < 0.01f;
}

bool sharp_offset_spread_and_order() {
    const Painted plain = paint(styled({{"box-shadow", "4px 6px 0 0 #000000"}}));
    const Painted spread = paint(styled({{"box-shadow", "0 0 0 5px #ff0000"}}));
    const Painted with_background =
        paint(styled({{"box-shadow", "2px 2px #000000"}, {"background-color", "#ffffff"}}));
    return expect(plain.ops.size() == 1 && plain.stats.shadows == 1, "one sharp fill") &&
           expect(near(plain.ops[0].rect.x, 14) && near(plain.ops[0].rect.y, 16) &&
                      near(plain.ops[0].rect.width, 50) && near(plain.ops[0].rect.height, 40),
                  "the shadow is the node box moved by the offset") &&
           expect(near(plain.ops[0].color.a, 1.0f), "the shadow keeps the color alpha") &&
           expect(spread.ops.size() == 1 && near(spread.ops[0].rect.x, 5) &&
                      near(spread.ops[0].rect.width, 60) && near(spread.ops[0].rect.height, 50),
                  "spread grows the box on every side") &&
           expect(with_background.ops.size() == 2 && with_background.ops[0].color.r < 0.1f &&
                      with_background.ops[1].color.r > 0.9f,
                  "the shadow is painted beneath the background");
}

bool radius_opacity_and_lists() {
    const Painted rounded = paint(styled({{"box-shadow", "0 0 0 2px #000000"},
                                          {"border-top-left-radius", "8px"},
                                          {"border-top-right-radius", "8px"},
                                          {"border-bottom-left-radius", "8px"},
                                          {"border-bottom-right-radius", "8px"}}));
    const Painted faded = paint(styled({{"box-shadow", "1px 1px #000000"}, {"opacity", "0.5"}}));
    const Painted two =
        paint(styled({{"box-shadow", "1px 1px #ff0000, 2px 2px rgba(0, 0, 255, 1)"}}));
    return expect(rounded.ops.size() == 1 && near(rounded.ops[0].radius, 10),
                  "spread adds to the node corner radius") &&
           expect(faded.ops.size() == 1 && near(faded.ops[0].color.a, 0.5f),
                  "node opacity multiplies the shadow alpha") &&
           expect(two.ops.size() == 2 && two.ops[0].color.b > 0.9f && two.ops[1].color.r > 0.9f,
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
    bool ok = expect(blurred.ops.size() >= 3 && blurred.stats.shadows == blurred.ops.size(),
                     "a blurred shadow is a stack of layers");
    if (!ok) {
        return false;
    }
    // Layers shrink monotonically, stay centered on the node, and the outermost reaches the blur.
    bool shrinking = true;
    for (std::size_t i = 1; i < blurred.ops.size(); ++i) {
        shrinking = shrinking && blurred.ops[i].rect.width < blurred.ops[i - 1].rect.width;
    }
    const Op& outer = blurred.ops.front();
    ok = expect(shrinking, "layers shrink toward the box") &&
         expect(near(outer.rect.x, 10 - 12) && near(outer.rect.width, 50 + 24),
                "the outermost layer reaches one blur radius out") &&
         ok;
    // The composite alpha inside the innermost layer equals the shadow color alpha (here 1), and
    // the composite in the outermost band is the faint tail of the Gaussian.
    float interior = 0.0f;
    for (const Op& op : blurred.ops) {
        interior = 1.0f - (1.0f - interior) * (1.0f - op.color.a);
    }
    const float tail = blurred.ops.front().color.a;
    ok = expect(near(interior, 1.0f), "the interior composites to the full shadow alpha") && ok;
    ok = expect(tail > 0.0f && tail < 0.2f, "the outer band is a faint tail") && ok;
    // Cumulative alpha rises monotonically from outside to inside.
    float cumulative = 0.0f;
    bool rising = true;
    for (const Op& op : blurred.ops) {
        const float next = 1.0f - (1.0f - cumulative) * (1.0f - op.color.a);
        rising = rising && next >= cumulative;
        cumulative = next;
    }
    return expect(rising, "coverage never decreases toward the interior") && ok;
}

bool reduced_effects_keep_the_reach_with_fewer_layers() {
    Node node(1);
    node.set_bounds({10, 10, 50, 40});
    StubStyles styles;
    styles.set(1, styled({{"box-shadow", "0 0 12px #000000"}}));
    RecordingSink full_sink;
    const PaintStats full = paint_scene(node, styles, full_sink);
    RecordingSink reduced_sink;
    tessera::ui::paint::PaintContext context;
    context.reduce_effects = true;
    const PaintStats reduced = paint_scene(node, styles, reduced_sink, context);
    bool ok = expect(reduced.shadows == 2 && reduced.shadows < full.shadows,
                     "reduced effects emit the capped number of blur layers");
    // The outermost layer is the shadow's full reach, so damage and the visible silhouette hold.
    ok = expect(!reduced_sink.ops.empty() && !full_sink.ops.empty() &&
                    near(reduced_sink.ops.front().rect.width, full_sink.ops.front().rect.width) &&
                    near(reduced_sink.ops.front().rect.height, full_sink.ops.front().rect.height),
                "the outer extent of a reduced shadow equals the full one") &&
         ok;
    // The innermost layer is still painted with a visible alpha: the shadow keeps its colour.
    ok = expect(reduced_sink.ops.back().color.a > 0.0f,
                "a reduced shadow still paints a visible innermost layer") &&
         ok;
    // A sharp shadow is already one fill: reduction changes nothing.
    styles.set(1, styled({{"box-shadow", "4px 4px 0 #000000"}}));
    RecordingSink sharp_full;
    RecordingSink sharp_reduced;
    const PaintStats a = paint_scene(node, styles, sharp_full);
    const PaintStats b = paint_scene(node, styles, sharp_reduced, context);
    ok =
        expect(a.shadows == 1 && b.shadows == 1, "a sharp shadow is unaffected by reduction") && ok;
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

int main() {
    bool ok = true;
    ok = sharp_offset_spread_and_order() && ok;
    ok = radius_opacity_and_lists() && ok;
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
