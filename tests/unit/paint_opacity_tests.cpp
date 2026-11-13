// SPDX-License-Identifier: MIT
//
// Effective opacity (issue #227): `opacity` is not an inherited CSS property, but its visual effect
// composites the node's whole subtree. The paint pass multiplies the ancestor product into every
// primitive (box visuals, text/glyph quads, vector/image tints) without an offscreen layer, marks
// an animated ancestor's descendants Paint|Render dirty so the subtree repaints, and keeps a fully
// transparent node hit-testable. The paint cache keys the inherited product so a record recorded
// under one ancestor opacity never replays under another.
//
// Deterministic: no clock, fonts, network or GPU.

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/animation/property_dirty.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"

#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Rect;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::Dirty;
using tessera::ui::DirtyPass;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::animation::AnimationInput;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::declarations_dirty_paint_subtree;
using tessera::ui::animation::FillMode;
using tessera::ui::animation::KeyframeTiming;
using tessera::ui::animation::paint_dirties_subtree;
using tessera::ui::frame::FramePipeline;
using tessera::ui::frame::FrameRender;
using tessera::ui::frame::FrameTick;
using tessera::ui::layout::DamageTracker;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintCache;
using tessera::ui::paint::PaintCacheStats;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::TextDraw;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_opacity: " << message << "\n";
    }
    return condition;
}

bool near(float actual, float expected, float tolerance = 1.0e-6f) {
    return std::abs(actual - expected) <= tolerance;
}

bool same_color(const Color& actual, const Color& expected, float tolerance = 1.0e-6f) {
    return near(actual.r, expected.r, tolerance) && near(actual.g, expected.g, tolerance) &&
           near(actual.b, expected.b, tolerance) && near(actual.a, expected.a, tolerance);
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

struct TextOp {
    Rect rect;
    std::string text;
    Color color;
};

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color& color) override {
        fills.push_back({rect, color});
    }

    void fill_rounded_rect(const Rect& rect, const Color& color, float /*radius*/) override {
        fills.push_back({rect, color});
    }

    void draw_text(const TextDraw& run) override {
        texts.push_back({run.rect, std::string(run.text), run.color});
    }

    std::vector<std::pair<Rect, Color>> fills;
    std::vector<TextOp> texts;
};

void set_text(Node& node, std::string text) {
    PaintData data;
    data.text = std::move(text);
    node.set_paint_data(std::move(data));
}

void clear_tree(Node& node) {
    node.clear_dirty(Dirty::All);
    for (const auto& child : node.children()) {
        clear_tree(*child);
    }
}

KeyframeBlock opacity_block() {
    KeyframeBlock block;
    block.name = "fade";
    block.steps.push_back(KeyframeStep{0.0, {Declaration{"opacity", "0", false, 0}}});
    block.steps.push_back(KeyframeStep{100.0, {Declaration{"opacity", "1", false, 0}}});
    return block;
}

KeyframeTiming timing() {
    KeyframeTiming result;
    result.duration = 0.1;
    return result;
}

// --- Paint semantics -----------------------------------------------------------------------------

bool parent_opacity_fades_text_and_background_child() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    Node& child = root.add_child(std::make_unique<Node>(2));
    child.set_bounds({10, 10, 100, 20});
    set_text(child, "Hi");

    StubStyles styles;
    styles.set(1, styled({{"opacity", "0.5"}, {"background-color", "white"}}));
    styles.set(2, styled({{"background-color", "rgba(0, 0, 255, 0.4)"}, {"color", "white"}}));

    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink);

    return expect(stats.backgrounds == 2, "parent and child backgrounds paint") &&
           expect(stats.texts == 1, "the child text run paints") &&
           expect(sink.fills.size() == 2, "two background quads") &&
           expect(same_color(sink.fills[0].second, Color{1.0f, 1.0f, 1.0f, 0.5f}),
                  "the parent background is at half alpha") &&
           expect(same_color(sink.fills[1].second, Color{0.0f, 0.0f, 1.0f, 0.2f}),
                  "the child background multiplies its own alpha by the ancestor product") &&
           expect(sink.texts.size() == 1 && sink.texts[0].text == "Hi",
                  "one text run with the payload text") &&
           expect(same_color(sink.texts[0].color, Color{1.0f, 1.0f, 1.0f, 0.5f}),
                  "the text child's glyph quads are at half alpha");
}

bool nested_opacity_is_the_product_of_the_chain() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    Node& middle = root.add_child(std::make_unique<Node>(2));
    middle.set_bounds({10, 10, 100, 100});
    Node& leaf = middle.add_child(std::make_unique<Node>(3));
    leaf.set_bounds({5, 5, 50, 50});

    StubStyles styles;
    styles.set(1, styled({{"opacity", "0.5"}}));
    styles.set(2, styled({{"opacity", "0.5"}, {"background-color", "red"}}));
    styles.set(3, styled({{"background-color", "blue"}}));

    RecordingSink sink;
    paint_scene(root, styles, sink);

    // 0.5 * 0.5 = 0.25 for the middle node, and the leaf inherits the same product with no opacity
    // of its own. Exact alpha equality: every factor is a power of two, so the product is exact.
    return expect(sink.fills.size() == 2, "middle and leaf backgrounds paint") &&
           expect(sink.fills[0].second.a == 0.25f, "nested 0.5 over 0.5 is exactly 0.25") &&
           expect(sink.fills[1].second.a == 0.25f,
                  "a descendant without its own opacity keeps the ancestor product") &&
           expect(same_color(sink.fills[1].second, Color{0.0f, 0.0f, 1.0f, 0.25f}),
                  "the leaf keeps its own colour and the product alpha");
}

bool zero_opacity_paints_nothing_and_stays_hit_testable() {
    Node root(1);
    root.set_bounds({0, 0, 100, 100});
    Node& child = root.add_child(std::make_unique<Node>(2));
    child.set_bounds({10, 10, 50, 50});
    set_text(child, "Hi");

    StubStyles styles;
    styles.set(1, styled({{"opacity", "0"}, {"background-color", "red"}}));
    styles.set(2, styled({{"background-color", "blue"}, {"color", "white"}}));

    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink);

    // CSS keeps a fully transparent element interactive: the scene is untouched and paint only
    // drops the emits, so hit testing still resolves both boxes.
    return expect(sink.fills.empty() && sink.texts.empty(), "opacity 0 emits nothing") &&
           expect(stats.backgrounds == 0 && stats.texts == 0, "no draw counters") &&
           expect(stats.nodes == 2, "the transparent subtree is still traversed") &&
           expect(root.hit_test(50.0f, 50.0f), "the transparent ancestor is still hit-testable") &&
           expect(child.hit_test(20.0f, 20.0f), "the transparent child is still hit-testable");
}

// --- Ancestor change invalidation ----------------------------------------------------------------

bool opacity_animation_repaints_the_subtree() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    Node& parent = root.add_child(std::make_unique<Node>(2));
    parent.set_bounds({10, 10, 100, 100});
    Node& child = parent.add_child(std::make_unique<Node>(3));
    child.set_bounds({5, 5, 50, 50});
    Node& leaf = child.add_child(std::make_unique<Node>(4));
    leaf.set_bounds({5, 5, 20, 20});
    clear_tree(root);

    ManualClock clock;
    RuntimeLoop loop(clock);
    AnimationRuntime animations;
    DamageAccumulator damage(DamageConfig{Extent2D{200, 200}, 8});
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipeline pipeline(animations, loop, tracker, damage, dirty);

    animations.start(AnimationInput{2, opacity_block(), timing(), {}, "", false});
    const FrameTick tick = pipeline.tick(root, 0.05);
    const FrameRender render = pipeline.finish_frame(root);

    if (!expect(tick.animation.samples.size() == 1, "one opacity sample") ||
        !expect(paint_dirties_subtree("opacity"), "opacity is the subtree-dirtying property") ||
        !expect(!paint_dirties_subtree("background-color"),
                "a colour animation only dirties its target") ||
        !expect(has_flag(parent.dirty(), Dirty::Paint), "the animated parent is paint dirty") ||
        !expect(has_flag(child.dirty(), Dirty::Paint | Dirty::Render),
                "the child is repainted by the parent's fade") ||
        !expect(has_flag(leaf.dirty(), Dirty::Paint | Dirty::Render),
                "the grandchild is repainted by the parent's fade") ||
        !expect(render.damage.nodes == 3, "damage covers the whole faded subtree") ||
        !expect(render.should_render && !render.full_redraw,
                "the subtree repaint is partial damage")) {
        return false;
    }

    StubStyles styles;
    styles.set(2, styled({{"opacity", "0.5"}, {"background-color", "red"}}));
    styles.set(3, styled({{"background-color", "blue"}}));
    styles.set(4, styled({{"color", "white"}}));
    set_text(leaf, "Hi");

    RecordingSink sink;
    const auto paint = pipeline.paint_frame(root, styles, sink);
    return expect(paint.painted, "the dirty frame paints") &&
           expect(sink.texts.size() == 1, "the text leaf paints") &&
           expect(same_color(sink.texts[0].color, Color{1.0f, 1.0f, 1.0f, 0.5f}),
                  "the repainted descendant carries the parent's effective opacity") &&
           expect(sink.fills.size() == 2, "parent and child backgrounds paint") &&
           expect(sink.fills[0].second.a == 0.5f && sink.fills[1].second.a == 0.5f,
                  "the subtree box visuals composite at the ancestor product");
}

bool paint_cache_does_not_replay_under_a_new_ancestor_opacity() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    Node& parent = root.add_child(std::make_unique<Node>(2));
    parent.set_bounds({10, 10, 100, 100});
    Node& child = parent.add_child(std::make_unique<Node>(3));
    child.set_bounds({5, 5, 50, 20});
    set_text(child, "Hi");
    clear_tree(root);

    StubStyles styles;
    styles.set(2, styled({{"opacity", "0.5"}, {"background-color", "red"}}));
    styles.set(3, styled({{"color", "white"}}));

    PaintCache cache;
    PaintContext context;
    context.paint_cache = &cache;

    RecordingSink first;
    paint_scene(root, styles, first, context);
    if (!expect(first.texts.size() == 1, "first paint emits the text") ||
        !expect(same_color(first.texts[0].color, Color{1.0f, 1.0f, 1.0f, 0.5f}),
                "first paint folds the parent opacity")) {
        return false;
    }

    // Only the parent is restyled (the child stays Clean), exactly like a parent fade whose
    // descendants did not change their own style.
    styles.set(2, styled({{"opacity", "0.25"}, {"background-color", "red"}}));
    parent.mark_dirty(Dirty::Paint | Dirty::Render);

    RecordingSink second;
    paint_scene(root, styles, second, context);
    const PaintCacheStats stats = cache.stats();
    return expect(second.texts.size() == 1, "second paint emits the text again") &&
           expect(same_color(second.texts[0].color, Color{1.0f, 1.0f, 1.0f, 0.25f}),
                  "the child record re-records instead of replaying the old product") &&
           expect(second.fills.size() == 1 && second.fills[0].second.a == 0.25f,
                  "the parent re-records at the new opacity") &&
           expect(stats.hits == 1,
                  "only the unchanged ancestor replays; the faded subtree re-records") &&
           expect(stats.misses > 0, "the subtree took the recording path");
}

} // namespace

int main() {
    bool ok = true;
    ok = parent_opacity_fades_text_and_background_child() && ok;
    ok = nested_opacity_is_the_product_of_the_chain() && ok;
    ok = zero_opacity_paints_nothing_and_stays_hit_testable() && ok;
    ok = opacity_animation_repaints_the_subtree() && ok;
    ok = paint_cache_does_not_replay_under_a_new_ancestor_opacity() && ok;

    if (!ok) {
        return 1;
    }
    std::cout << "paint_opacity: ok\n";
    return 0;
}
