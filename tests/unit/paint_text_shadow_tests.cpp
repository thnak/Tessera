// SPDX-License-Identifier: MIT
//
// `text-shadow` (issue #616): the list grammar, the paint order (every shadow behind every glyph of
// the block, including inline spans), the bounded blur approximation, the opacity fold, the
// `reduce_effects` cap, and the two invariants the issue calls out - layout and the glyph ink box
// are untouched, and the style's reach lands in the node's paint outsets so culling and damage stay
// correct.

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/paint/scene_style_sync.hpp"
#include "tessera/ui/text/text_layout.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::RoundedClip;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::TextDraw;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_text_shadow: " << message << "\n";
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

struct RecordedText {
    Rect rect;
    std::string text;
    Color color;
    bool lcd = false;
    std::optional<float> baseline;
};

// Records every text run in emission order, so "shadow before glyph" is an ordering assertion and
// not just a count.
class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect&, const Color&) override {}
    void fill_rounded_rect(const Rect&, const Color&, float) override {}
    void push_clip(const RoundedClip&) override {}
    void pop_clip() override {}
    void draw_text(const TextDraw& run) override {
        texts.push_back(
            RecordedText{run.rect, std::string(run.text), run.color, run.lcd, run.baseline});
    }

    std::vector<RecordedText> texts;
};

PaintData text_run(std::string text) {
    PaintData data;
    data.text = std::move(text);
    return data;
}

// A node with one text payload and a style, painted once.
PaintStats paint_one(Node& node, StubStyles& styles, PaintSink& sink) {
    return paint_scene(node, styles, sink);
}

bool test_parse_grammar() {
    // The grammar is `[<color>? <x> <y> <blur>?]`, comma separated, colour in either position.
    const auto parse = [](const char* value) {
        Node root(1);
        root.set_bounds({0, 0, 100, 20});
        StubStyles styles;
        styles.set(1, styled({{"text-shadow", value}, {"color", "red"}}));
        root.set_paint_data(text_run("Hi"));
        RecordingSink sink;
        paint_one(root, styles, sink);
        return sink.texts;
    };

    // `none` and a missing property paint no shadow: exactly one run, the glyphs.
    const auto none = parse("none");
    bool ok = expect(none.size() == 1, "none paints only the glyph run");

    // A sharp shadow is one offset copy plus the glyphs, shadow first.
    const auto sharp = parse("2px 3px black");
    ok = expect(sharp.size() == 2, "a sharp shadow is one copy plus the glyphs") && ok;
    ok = expect(sharp.size() == 2 && sharp[0].rect.x == sharp[1].rect.x + 2.0f &&
                    sharp[0].rect.y == sharp[1].rect.y + 3.0f,
                "the shadow copy is offset by the declared x/y") &&
         ok;
    ok = expect(sharp.size() == 2 && sharp[0].text == "Hi" && sharp[1].text == "Hi",
                "the shadow copy carries the same glyphs") &&
         ok;

    // Colour in the leading position, and `currentColor` resolving to the node's `color`.
    const auto leading = parse("rgba(0, 0, 255, 0.5) 1px 1px");
    ok = expect(leading.size() == 2 && std::abs(leading[0].color.b - 1.0f) < 0.01f &&
                    std::abs(leading[0].color.a - 0.5f) < 0.01f,
                "a leading colour is parsed") &&
         ok;
    const auto current = parse("1px 1px currentColor");
    ok = expect(current.size() == 2 && current[0].color.r > 0.9f && current[0].color.g < 0.1f,
                "currentColor resolves to the node colour") &&
         ok;

    // A list keeps every layer, painted back to front (the first shadow on top).
    const auto list = parse("1px 1px black, 2px 2px black");
    ok = expect(list.size() == 3, "two sharp shadows are two copies plus the glyphs") && ok;
    ok = expect(list.size() == 3 && list[0].rect.x == list[2].rect.x + 2.0f &&
                    list[1].rect.x == list[2].rect.x + 1.0f,
                "the list paints back to front") &&
         ok;

    // Invalid forms paint nothing at all (the declaration is invalid as a whole).
    ok = expect(parse("1px").size() == 1, "one length is invalid") && ok;
    ok = expect(parse("inset 1px 1px black").size() == 1, "text-shadow has no inset form") && ok;
    ok =
        expect(parse("1px 1px 2px 3px black").size() == 1, "a fourth length (spread) is invalid") &&
        ok;
    ok = expect(parse("1px 1px 2px black, garbage").size() == 1,
                "one invalid layer invalidates the list") &&
         ok;
    ok = expect(parse("1px 1px -2px black").size() == 1, "a negative blur is invalid") && ok;
    return ok;
}

bool test_shadow_is_behind_every_glyph() {
    // A wrapped paragraph emits one run per line. Every shadow must precede every glyph run, or a
    // later line's shadow would darken an earlier line's glyphs.
    Node root(1);
    root.set_bounds({0, 0, 60, 200});
    StubStyles styles;
    styles.set(1, styled({{"text-shadow", "1px 1px black"}, {"font-size", "16px"}}));
    PaintData data = text_run("alpha beta gamma delta epsilon zeta");
    data.wrap = tessera::ui::PaintWrap::Wrap;
    root.set_paint_data(std::move(data));
    RecordingSink sink;
    paint_one(root, styles, sink);

    std::size_t glyph_runs = 0;
    std::size_t shadow_runs = 0;
    std::size_t last_shadow = 0;
    std::size_t first_glyph = sink.texts.size();
    // The block is [all shadows][all glyphs]: the shadow copies are the first half.
    if (sink.texts.size() % 2 == 0) {
        glyph_runs = sink.texts.size() / 2;
        shadow_runs = glyph_runs;
        last_shadow = shadow_runs - 1;
        first_glyph = shadow_runs;
    }
    bool ok = expect(glyph_runs > 1, "the paragraph wraps into several lines");
    ok = expect(shadow_runs == glyph_runs, "every line gets its own shadow copy") && ok;
    ok = expect(last_shadow < first_glyph, "every shadow precedes every glyph run") && ok;
    // The shadow block is offset by exactly the declared amount from the glyph block.
    for (std::size_t i = 0; i < glyph_runs; ++i) {
        ok = expect(sink.texts[i].rect.x == sink.texts[first_glyph + i].rect.x + 1.0f &&
                        sink.texts[i].rect.y == sink.texts[first_glyph + i].rect.y + 1.0f,
                    "each shadow copy matches its line's offset") &&
             ok;
    }
    return ok;
}

bool test_inline_spans_get_shadows() {
    // A span-attributed Text paints one run per span slice; the shadow must cover those runs too,
    // which is what the sink wrapper buys over a per-call-site implementation.
    Node root(1);
    root.set_bounds({0, 0, 200, 40});
    StubStyles styles;
    styles.set(1, styled({{"text-shadow", "1px 1px black"}, {"font-size", "16px"}}));
    PaintData data;
    data.text = "plain bold";
    data.spans.push_back(tessera::ui::PaintSpan{2, 6, 10});
    root.set_paint_data(std::move(data));
    auto span = std::make_unique<Node>(2);
    span->set_bounds({0, 0, 40, 20});
    root.add_child(std::move(span));
    styles.set(2, styled({{"font-weight", "700"}}));

    RecordingSink sink;
    paint_one(root, styles, sink);
    // Two slices (unattributed + span) means two glyph runs and two shadow copies.
    bool ok = expect(sink.texts.size() == 4, "two span slices produce two shadow copies");
    ok = expect(sink.texts.size() == 4 && sink.texts[0].rect.x == sink.texts[2].rect.x + 1.0f &&
                    sink.texts[1].rect.x == sink.texts[3].rect.x + 1.0f,
                "each span slice's shadow precedes the slice") &&
         ok;
    return ok;
}

bool test_blur_is_bounded_and_fades() {
    // A blurred shadow is a bounded stack of offset copies: more than a sharp one, never unbounded,
    // and the copies' alphas rise toward the centre so the edge fades.
    const auto copies = [](const char* value, bool reduce) {
        Node root(1);
        root.set_bounds({0, 0, 100, 20});
        StubStyles styles;
        styles.set(1, styled({{"text-shadow", value}}));
        root.set_paint_data(text_run("Hi"));
        RecordingSink sink;
        tessera::ui::paint::PaintContext context;
        context.reduce_effects = reduce;
        paint_scene(root, styles, sink, context);
        return sink.texts;
    };
    const auto full = copies("0 0 8px black", false);
    const auto reduced = copies("0 0 8px black", true);
    // 4 rings * 8 samples + the centred copy, plus the glyph run.
    constexpr std::size_t kMaxCopies = 4 * 8 + 1;
    bool ok = expect(full.size() > 2, "a blurred shadow is more than one copy");
    ok = expect(full.size() <= kMaxCopies + 1, "the blurred stack is bounded") && ok;
    ok = expect(reduced.size() < full.size(), "reduce_effects emits fewer copies") && ok;
    ok = expect(reduced.size() >= 2, "reduce_effects still paints a shadow") && ok;
    // The last copy before the glyphs is the centred full-alpha one; the outermost ring is faint.
    ok = expect(full.size() >= 3 && full[0].color.a < full[full.size() - 2].color.a,
                "the outer ring is fainter than the centre") &&
         ok;
    return ok;
}

bool test_opacity_folds_into_shadow() {
    Node root(1);
    root.set_bounds({0, 0, 100, 20});
    StubStyles styles;
    styles.set(1, styled({{"text-shadow", "1px 1px rgba(0, 0, 0, 0.8)"}, {"opacity", "0.5"}}));
    root.set_paint_data(text_run("Hi"));
    RecordingSink sink;
    paint_one(root, styles, sink);
    return expect(sink.texts.size() == 2, "one shadow copy plus the glyphs") &&
           expect(sink.texts.size() == 2 && std::abs(sink.texts[0].color.a - 0.4f) < 0.01f,
                  "node opacity multiplies the shadow alpha") &&
           expect(sink.texts.size() == 2 && std::abs(sink.texts[1].color.a - 0.5f) < 0.01f,
                  "the glyph alpha keeps its own opacity fold");
}

bool test_shadow_never_marks_lcd() {
    // A shadow is a blurred impression of the glyphs; per-channel LCD coverage would put colour
    // fringes on it, so a shadow copy is never LCD-eligible even when the run is.
    Node root(1);
    root.set_bounds({0, 0, 100, 20});
    StubStyles styles;
    styles.set(1, styled({{"text-shadow", "1px 1px black"}}));
    root.set_paint_data(text_run("Hi"));
    RecordingSink sink;
    tessera::ui::paint::PaintContext context;
    context.lcd_text = true;
    paint_scene(root, styles, sink, context);
    return expect(sink.texts.size() == 2, "one shadow copy plus the glyphs") &&
           expect(sink.texts.size() == 2 && !sink.texts[0].lcd, "the shadow copy is not LCD") &&
           expect(sink.texts.size() == 2 && sink.texts[1].lcd, "the glyph run stays LCD");
}

bool test_layout_and_ink_box_unchanged() {
    // The issue's hard requirement: a shadow must not change layout or the glyph ink measurement.
    // Paint is read-only over the scene, so the box and the payload are byte-identical with and
    // without a shadow, and the emitted glyph run keeps the unshadowed rect.
    const auto paint = [](const char* shadow, Rect& box, Rect& glyph_rect) {
        Node root(1);
        root.set_bounds({0, 0, 120, 30});
        StubStyles styles;
        styles.set(1, styled({{"text-shadow", shadow}, {"font-size", "16px"}}));
        root.set_paint_data(text_run("Hello"));
        RecordingSink sink;
        paint_one(root, styles, sink);
        box = root.bounds();
        // The glyph run is the last one emitted (shadows come first).
        glyph_rect = sink.texts.empty() ? Rect{} : sink.texts.back().rect;
        return sink.texts.size();
    };
    Rect plain_box{};
    Rect plain_glyph{};
    Rect shadowed_box{};
    Rect shadowed_glyph{};
    const std::size_t plain = paint("none", plain_box, plain_glyph);
    const std::size_t shadowed = paint("4px 4px 6px black", shadowed_box, shadowed_glyph);
    return expect(plain == 1, "no shadow paints one run") &&
           expect(shadowed > plain, "a shadow adds runs") &&
           expect(plain_box.x == shadowed_box.x && plain_box.y == shadowed_box.y &&
                      plain_box.width == shadowed_box.width &&
                      plain_box.height == shadowed_box.height,
                  "the node box is unchanged by a shadow") &&
           expect(plain_glyph.x == shadowed_glyph.x && plain_glyph.y == shadowed_glyph.y &&
                      plain_glyph.width == shadowed_glyph.width &&
                      plain_glyph.height == shadowed_glyph.height,
                  "the glyph run rect (the ink box) is unchanged by a shadow");
}

bool test_outsets_carry_the_reach() {
    // The reach is a style fact, so the scene sync folds it into the node's paint outsets: that is
    // what keeps culling and damage correct without growing the ink box.
    Node root(1);
    root.set_bounds({0, 0, 100, 20});
    StubStyles styles;
    styles.set(1, styled({{"text-shadow", "3px 4px 6px black"}}));
    tessera::ui::paint::sync_scene_style(root, styles, {});
    const Node::PaintOutsets& outsets = root.paint_outsets();
    bool ok = expect(std::abs(outsets.right - 9.0f) < 0.01f, "right reach is blur + offset x");
    ok = expect(std::abs(outsets.bottom - 10.0f) < 0.01f, "bottom reach is blur + offset y") && ok;
    ok = expect(std::abs(outsets.left - 3.0f) < 0.01f, "left reach is blur - offset x") && ok;
    ok = expect(std::abs(outsets.top - 2.0f) < 0.01f, "top reach is blur - offset y") && ok;
    ok = expect(outsets.blurred, "a blurred text shadow marks the outsets blurred") && ok;

    // `none` leaves the outsets at zero, so an unshadowed node is unchanged.
    Node plain(2);
    plain.set_bounds({0, 0, 100, 20});
    StubStyles plain_styles;
    plain_styles.set(2, styled({{"text-shadow", "none"}}));
    tessera::ui::paint::sync_scene_style(plain, plain_styles, {});
    ok = expect(plain.paint_outsets().right == 0.0f && plain.paint_outsets().bottom == 0.0f,
                "none contributes no reach") &&
         ok;
    return ok;
}

bool test_no_text_payload_paints_no_shadow() {
    // A node with a shadow but no text payload emits nothing: the shadow is a property of the
    // glyphs, not a box effect.
    Node root(1);
    root.set_bounds({0, 0, 100, 20});
    StubStyles styles;
    styles.set(1, styled({{"text-shadow", "1px 1px black"}, {"background-color", "black"}}));
    RecordingSink sink;
    const PaintStats stats = paint_one(root, styles, sink);
    return expect(sink.texts.empty(), "no payload paints no shadow") &&
           expect(stats.text_shadows == 0, "the text-shadow counter stays zero");
}

bool test_stats_count_copies() {
    Node root(1);
    root.set_bounds({0, 0, 100, 20});
    StubStyles styles;
    styles.set(1, styled({{"text-shadow", "1px 1px black, 2px 2px black"}}));
    root.set_paint_data(text_run("Hi"));
    RecordingSink sink;
    const PaintStats stats = paint_one(root, styles, sink);
    return expect(stats.text_shadows == 2, "two sharp shadows count two copies") &&
           expect(stats.texts == 1, "the glyph run is counted once");
}

} // namespace

int main() {
    bool ok = true;
    ok = test_parse_grammar() && ok;
    ok = test_shadow_is_behind_every_glyph() && ok;
    ok = test_inline_spans_get_shadows() && ok;
    ok = test_blur_is_bounded_and_fades() && ok;
    ok = test_opacity_folds_into_shadow() && ok;
    ok = test_shadow_never_marks_lcd() && ok;
    ok = test_layout_and_ink_box_unchanged() && ok;
    ok = test_outsets_carry_the_reach() && ok;
    ok = test_no_text_payload_paints_no_shadow() && ok;
    ok = test_stats_count_copies() && ok;

    if (!ok) {
        std::cerr << "paint_text_shadow: failed\n";
        return 1;
    }
    std::cout << "paint_text_shadow: ok\n";
    return 0;
}
