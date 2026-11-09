// SPDX-License-Identifier: MIT
//
// Paint-side text runs (D9): PaintData on a node, emission order after background/borders and
// inside the active clip, style precedence, empty text, truncation/UTF-8 bound, dirty domains, and
// damage-clipped text replay through the frame pipeline.

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/paint/paint_pass.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::RedrawPlan;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Rect;
using tessera::rendering::rhi::rgba8;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::Dirty;
using tessera::ui::DirtyPass;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;
using tessera::ui::PaintTextStyle;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::frame::FramePaint;
using tessera::ui::frame::FramePipeline;
using tessera::ui::layout::DamageTracker;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::RoundedClip;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::TextDraw;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_text: " << message << "\n";
    }
    return condition;
}

PaintData text_run(std::string text) {
    PaintData data;
    data.text = std::move(text);
    return data;
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
    float font_size = 0.0f;
    int font_weight = 0;
    Color color;
    int active_clips = 0;
};

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect&, const Color&) override {
        ++fills;
    }
    void fill_rounded_rect(const Rect&, const Color&, float) override {
        ++fills;
    }
    void push_clip(const RoundedClip& clip) override {
        clips.push_back(clip);
        ++active_clips;
        ++clip_pushes;
    }
    void pop_clip() override {
        --active_clips;
        ++clip_pops;
    }
    void draw_text(const TextDraw& run) override {
        texts.push_back(RecordedText{run.rect, std::string(run.text), run.font_size,
                                     run.font_weight, run.color, active_clips});
        text_after_fills.push_back(fills);
    }

    std::size_t fills = 0;
    std::vector<RoundedClip> clips;
    int active_clips = 0;
    int clip_pushes = 0;
    int clip_pops = 0;
    std::vector<RecordedText> texts;
    std::vector<std::size_t> text_after_fills;
};

bool same_rect(const Rect& actual, const Rect& expected) {
    return actual.x == expected.x && actual.y == expected.y && actual.width == expected.width &&
           actual.height == expected.height;
}

bool test_emit_order_and_style() {
    Node root(1);
    root.set_bounds({0, 0, 100, 40});
    StubStyles styles;
    styles.set(1, styled({{"background-color", "black"},
                          {"overflow-x", "hidden"},
                          {"overflow-y", "hidden"},
                          {"font-size", "20px"},
                          {"font-weight", "700"},
                          {"color", "blue"}}));
    root.set_paint_data(text_run("Hello"));

    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink);
    return expect(stats.texts == 1, "one text run emitted") &&
           expect(sink.texts.size() == 1, "sink saw one text run") &&
           expect(sink.text_after_fills.size() == 1 && sink.text_after_fills[0] == 1,
                  "text is emitted after the background") &&
           expect(sink.texts[0].active_clips == 1 && sink.clip_pushes == 1,
                  "text is emitted inside the node's overflow clip") &&
           expect(same_rect(sink.texts[0].rect, {0, 0, 100, 40}), "text rect is the border box") &&
           expect(sink.texts[0].text == "Hello", "text content is passed through") &&
           expect(sink.texts[0].font_size == 20.0f, "font size comes from computed style") &&
           expect(sink.texts[0].font_weight == 700, "font weight comes from computed style") &&
           expect(sink.texts[0].color.r == 0.0f && sink.texts[0].color.b == 1.0f,
                  "color comes from computed style") &&
           expect(sink.clip_pops == 1, "clip is popped after the children");
}

bool test_style_precedence_and_opacity() {
    Node root(1);
    root.set_bounds({0, 0, 50, 20});
    StubStyles styles;
    styles.set(1, styled({{"background-color", "black"},
                          {"font-size", "12px"},
                          {"font-weight", "400"},
                          {"color", "blue"},
                          {"opacity", "0.5"}}));
    PaintData data;
    data.text = "Hi";
    data.text_style.font_size = 33.0f;
    data.text_style.font_weight = 300;
    data.text_style.color = rgba8(255, 0, 0, 255);
    root.set_paint_data(std::move(data));

    RecordingSink sink;
    paint_scene(root, styles, sink);
    return expect(sink.texts.size() == 1, "override run emitted") &&
           expect(sink.texts[0].font_size == 33.0f, "PaintData font size override wins") &&
           expect(sink.texts[0].font_weight == 300, "PaintData weight override wins") &&
           expect(sink.texts[0].color.r == 1.0f && sink.texts[0].color.g == 0.0f,
                  "PaintData color override wins") &&
           expect(std::abs(sink.texts[0].color.a - 0.5f) <= 1.0e-4f,
                  "node opacity multiplies the text alpha");
}

bool test_empty_and_missing_text() {
    Node root(1);
    root.set_bounds({0, 0, 40, 20});
    StubStyles styles;
    styles.set(1, styled({{"background-color", "black"}}));

    RecordingSink sink;
    PaintStats stats = paint_scene(root, styles, sink);
    bool ok = expect(stats.texts == 0 && sink.texts.empty(), "no PaintData paints no text");

    root.set_paint_data(text_run(""));
    stats = paint_scene(root, styles, sink);
    ok = expect(stats.texts == 0 && sink.texts.empty(), "empty text paints nothing") && ok;
    return ok && expect(sink.fills == 2, "background still paints for both runs");
}

bool test_child_text_under_parent_clip() {
    Node root(1);
    root.set_bounds({0, 0, 100, 40});
    auto child_owner = std::make_unique<Node>(2);
    child_owner->set_bounds({10, 10, 60, 20});
    Node& child = root.add_child(std::move(child_owner));
    child.set_paint_data(text_run("child"));

    StubStyles styles;
    styles.set(1, styled({{"overflow-x", "hidden"}, {"overflow-y", "hidden"}}));
    styles.set(2, styled({{"color", "white"}}));

    RecordingSink sink;
    paint_scene(root, styles, sink);
    return expect(sink.texts.size() == 1, "child text emitted") &&
           expect(sink.texts[0].active_clips == 1, "child text sees the parent clip") &&
           expect(same_rect(sink.texts[0].rect, {10, 10, 60, 20}), "child text rect");
}

bool test_paint_data_node_contract() {
    Node node(7);
    node.clear_dirty(Dirty::All);

    PaintData data;
    data.text = "A";
    node.set_paint_data(data);
    bool ok =
        expect(node.paint_data().has_value(), "paint data stored") &&
        expect(has_flag(node.dirty(), Dirty::Paint) && has_flag(node.dirty(), Dirty::Render),
               "paint data marks Paint|Render") &&
        expect(!has_flag(node.dirty(), Dirty::Layout) && !has_flag(node.dirty(), Dirty::Style),
               "paint data does not mark layout/style");

    node.clear_dirty(Dirty::All);
    node.set_paint_data(data);
    ok = expect(node.dirty() == Dirty::None, "identical paint data is a no-op") && ok;

    PaintData longer;
    longer.text = std::string(3000, 'x') + "\xC3\xA9" + std::string(3000, 'y');
    node.set_paint_data(std::move(longer));
    const std::string& stored = node.paint_data()->text;
    ok = expect(stored.size() <= PaintData::kMaxTextBytes, "text is bounded") && ok;
    ok = expect((static_cast<unsigned char>(stored.back()) & 0xC0U) != 0x80U,
                "truncation does not split a UTF-8 code point") &&
         ok;

    node.set_paint_data(std::nullopt);
    ok = expect(!node.paint_data().has_value(), "paint data can be cleared") && ok;
    node.clear_dirty(Dirty::All);
    node.clear_paint_data();
    ok = expect(node.dirty() == Dirty::None, "clearing empty paint data is a no-op") && ok;
    return ok;
}

bool test_damage_clipped_text() {
    Node root(1);
    root.set_bounds({0, 0, 100, 40});
    root.set_paint_data(text_run("damage"));
    StubStyles styles;
    styles.set(1, styled({{"background-color", "black"}, {"color", "white"}}));
    root.clear_dirty(Dirty::All);
    root.mark_dirty(Dirty::Paint);

    ManualClock clock;
    RuntimeLoop loop(clock);
    AnimationRuntime animations;
    // A target much larger than the dirty node keeps the damage plan partial (a full-redraw plan
    // intentionally bypasses the clipping decorator).
    DamageAccumulator damage(DamageConfig{Extent2D{400, 200}, 8});
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipeline pipeline(animations, loop, tracker, damage, dirty);

    pipeline.tick(root, 0.016);
    const auto render = pipeline.finish_frame(root);
    if (!expect(render.should_render && !render.full_redraw && !render.redraw_plan.scissors.empty(),
                "dirty text frame is a partial redraw")) {
        return false;
    }

    RecordingSink sink;
    const FramePaint painted = pipeline.paint_frame(root, styles, sink, render.redraw_plan);
    return expect(painted.painted, "damage-clipped paint runs") &&
           expect(sink.texts.size() == 1, "damage-clipped sink replays the text once") &&
           expect(sink.clip_pushes >= 1, "damage-clipped text is clipped to the damage region") &&
           expect(sink.clip_pushes == sink.clip_pops, "damage text clips are balanced") &&
           expect(same_rect(sink.texts[0].rect, {0, 0, 100, 40}), "damage text keeps its origin");
}

} // namespace

int main() {
    bool ok = true;
    ok = test_emit_order_and_style() && ok;
    ok = test_style_precedence_and_opacity() && ok;
    ok = test_empty_and_missing_text() && ok;
    ok = test_child_text_under_parent_clip() && ok;
    ok = test_paint_data_node_contract() && ok;
    ok = test_damage_clipped_text() && ok;

    if (!ok) {
        std::cerr << "paint_text: failed\n";
        return 1;
    }
    std::cout << "paint_text: ok\n";
    return 0;
}
