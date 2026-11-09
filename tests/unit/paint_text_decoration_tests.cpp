// SPDX-License-Identifier: MIT
//
// Paint caret/selection decoration: byte offsets -> line-box rectangles, z-order (selection behind
// glyphs, caret on top), width/colour rules, blink visibility (no timers), clip interaction, damage
// replay and empty-text behavior. Stub metrics keep the geometry deterministic.

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
using tessera::ui::PaintTextAlign;
using tessera::ui::PaintWrap;
using tessera::ui::TextDecoration;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::frame::FramePaint;
using tessera::ui::frame::FramePipeline;
using tessera::ui::layout::DamageTracker;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::RoundedClip;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::TextDraw;
using tessera::ui::style::ComputedStyle;

using tessera::ui::text::FontMetrics;
using tessera::ui::text::StubFontMetricsProvider;
using tessera::ui::text::TextStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_text_decoration: " << message << "\n";
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

enum class EventKind { Fill, Text };

struct Event {
    EventKind kind = EventKind::Fill;
    Rect rect;
    Color color;
    std::string text;
    int active_clips = 0;
};

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color& color) override {
        events.push_back(Event{EventKind::Fill, rect, color, {}, active_clips});
    }
    void fill_rounded_rect(const Rect& rect, const Color& color, float) override {
        fill_rect(rect, color);
    }
    void push_clip(const RoundedClip&) override {
        ++active_clips;
        ++clip_pushes;
    }
    void pop_clip() override {
        --active_clips;
        ++clip_pops;
    }
    void draw_text(const TextDraw& run) override {
        events.push_back(
            Event{EventKind::Text, run.rect, run.color, std::string(run.text), active_clips});
        ++texts;
    }
    const Event* first(EventKind kind) const {
        for (const Event& event : events) {
            if (event.kind == kind) {
                return &event;
            }
        }
        return nullptr;
    }
    std::size_t count(EventKind kind) const {
        std::size_t result = 0;
        for (const Event& event : events) {
            result += event.kind == kind ? 1U : 0U;
        }
        return result;
    }

    std::vector<Event> events;
    int active_clips = 0;
    int clip_pushes = 0;
    int clip_pops = 0;
    std::size_t texts = 0;
};

PaintData text_payload(std::string text, PaintWrap wrap = PaintWrap::NoWrap) {
    PaintData data;
    data.text = std::move(text);
    data.wrap = wrap;
    data.text_style.font_size = 16.0f;
    return data;
}

TextDecoration decoration(std::size_t caret, std::size_t begin = 0, std::size_t end = 0) {
    TextDecoration value;
    value.caret_byte_offset = caret;
    value.selection_begin = begin;
    value.selection_end = end;
    return value;
}

ComputedStyle decoration_style() {
    return styled({{"font-size", "16px"}, {"color", "white"}});
}

// Stub-metrics line geometry for expected checks (advance is 8 px/code point, line height from the
// same stub metrics the paint path uses).
struct ExpectedLine {
    Rect rect;
    float top = 0.0f;
    float height = 0.0f;
};

std::vector<ExpectedLine> expected_lines(const PaintData& data, float content_x, float content_y,
                                         float content_width, bool wrap, PaintTextAlign align) {
    TextStyle style;
    style.font_size = data.text_style.font_size;
    const FontMetrics metrics = StubFontMetricsProvider{}.metrics(style);
    tessera::ui::text::TextConstraints constraints;
    constraints.wrap = wrap;
    constraints.max_width = content_width;
    const auto layout = tessera::ui::text::layout_text(data.text, style, metrics, constraints);
    std::vector<ExpectedLine> result;
    float top = content_y;
    for (const auto& line : layout.lines) {
        float x = content_x;
        if (align == PaintTextAlign::Center) {
            x += (content_width - line.width) * 0.5f;
        } else if (align == PaintTextAlign::End) {
            x += content_width - line.width;
        }
        result.push_back(ExpectedLine{
            {std::round(std::max(content_x, x)), std::round(top), line.width, line.height},
            top,
            line.height});
        top += line.height;
    }
    return result;
}

bool test_no_wrap_caret_positions() {
    Node node(1);
    node.set_bounds({10, 20, 200, 40});
    StubStyles styles;
    styles.set(1, decoration_style());

    bool ok = true;
    for (const std::size_t offset : {std::size_t{0}, std::size_t{2}, std::size_t{5}}) {
        PaintData data = text_payload("abcde");
        data.decoration = decoration(offset);
        node.set_paint_data(std::move(data));
        RecordingSink sink;
        paint_scene(node, styles, sink);
        const Event* caret = sink.first(EventKind::Fill);
        ok = expect(caret != nullptr && sink.count(EventKind::Fill) == 1, "caret emits one fill") &&
             ok;
        if (caret != nullptr) {
            ok = expect(caret->rect.x == static_cast<float>(10 + static_cast<int>(offset) * 8),
                        "caret x follows the stub advance from the content start") &&
                 ok;
            ok = expect(caret->rect.width == 1.0f, "caret is 1 px at device scale 1") && ok;
            ok = expect(caret->color.a == 1.0f, "caret uses the text color at full alpha") && ok;
        }
        const Event* text = sink.first(EventKind::Text);
        ok = expect(text != nullptr && sink.count(EventKind::Text) == 1,
                    "decorated no-wrap text emits one run") &&
             ok;
    }
    return ok;
}

bool test_selection_rect_and_z_order() {
    Node node(1);
    node.set_bounds({10, 20, 200, 40});
    StubStyles styles;
    styles.set(1, decoration_style());
    PaintData data = text_payload("abcdef");
    data.decoration = decoration(4, 1, 4);
    node.set_paint_data(std::move(data));

    RecordingSink sink;
    paint_scene(node, styles, sink);
    bool ok = expect(sink.events.size() >= 3, "selection, text and caret emit events");
    const Event& selection = sink.events[0];
    ok = expect(selection.kind == EventKind::Fill && selection.rect.x == 18.0f &&
                    selection.rect.width == 24.0f,
                "selection rect covers the selected byte range") &&
         ok;
    ok = expect(std::abs(selection.color.a - 0.35f) < 1e-4f,
                "selection uses the documented alpha") &&
         ok;
    const Event& text = sink.events[1];
    ok = expect(text.kind == EventKind::Text, "text glyph run comes after the highlight") && ok;
    const Event& caret = sink.events.back();
    ok = expect(caret.kind == EventKind::Fill && caret.rect.x == 42.0f,
                "caret is painted on top at the caret offset") &&
         ok;
    return ok;
}

bool test_collapsed_and_blink() {
    Node node(1);
    node.set_bounds({0, 0, 120, 40});
    StubStyles styles;
    styles.set(1, decoration_style());

    PaintData collapsed = text_payload("abc");
    collapsed.decoration = decoration(1, 1, 1);
    node.set_paint_data(std::move(collapsed));
    RecordingSink sink;
    paint_scene(node, styles, sink);
    bool ok = expect(sink.count(EventKind::Fill) == 1, "collapsed selection paints only the caret");

    PaintData hidden = text_payload("abc");
    hidden.decoration = decoration(1);
    hidden.decoration->blink_phase = 0.2f;
    node.set_paint_data(std::move(hidden));
    RecordingSink blink;
    paint_scene(node, styles, blink);
    ok = expect(blink.count(EventKind::Fill) == 0, "blink phase below 0.5 hides the caret") && ok;

    PaintData invisible = text_payload("abc");
    invisible.decoration = decoration(1);
    invisible.decoration->caret_visible = false;
    node.set_paint_data(std::move(invisible));
    RecordingSink no_caret;
    paint_scene(node, styles, no_caret);
    ok = expect(no_caret.count(EventKind::Fill) == 0, "caret_visible=false hides the caret") && ok;
    return ok;
}

bool test_multi_line_selection() {
    Node node(1);
    node.set_bounds({0, 0, 64, 120});
    StubStyles styles;
    styles.set(1, decoration_style());
    PaintData data = text_payload("one two three four five six", PaintWrap::Wrap);
    data.decoration = decoration(8, 0, 27);
    node.set_paint_data(std::move(data));

    RecordingSink sink;
    paint_scene(node, styles, sink);
    const auto lines =
        expected_lines(*node.paint_data(), 0.0f, 0.0f, 64.0f, true, PaintTextAlign::Start);
    bool ok = expect(lines.size() > 1, "paragraph wraps");
    ok = expect(sink.count(EventKind::Fill) >= lines.size(),
                "selection paints one highlight per covered line") &&
         ok;
    ok =
        expect(sink.count(EventKind::Text) == lines.size(), "one glyph run per wrapped line") && ok;
    for (const auto& line : lines) {
        bool found = false;
        for (const Event& event : sink.events) {
            if (event.kind == EventKind::Fill && event.rect.y == line.rect.y &&
                event.rect.height == line.rect.height) {
                found = true;
                ok =
                    expect(event.rect.x >= line.rect.x && event.rect.x + event.rect.width <=
                                                              line.rect.x + line.rect.width + 0.01f,
                           "selection stays inside its line box") &&
                    ok;
            }
        }
        ok = expect(found, "each line box has a selection rect") && ok;
    }
    return ok;
}

bool test_newline_and_end_mapping() {
    Node node(1);
    node.set_bounds({0, 0, 100, 120});
    StubStyles styles;
    styles.set(1, decoration_style());

    bool ok = true;
    for (const std::size_t offset : {std::size_t{2}, std::size_t{3}}) {
        PaintData data = text_payload("ab\ncd", PaintWrap::Wrap);
        data.decoration = decoration(offset, 0, 5);
        node.set_paint_data(std::move(data));
        RecordingSink sink;
        paint_scene(node, styles, sink);
        const Event& caret = sink.events.back();
        ok = expect(caret.kind == EventKind::Fill, "caret emitted") && ok;
        if (offset == 2) {
            ok = expect(caret.rect.x == 16.0f && caret.rect.y == 0.0f,
                        "caret at a newline sits at the end of the preceding line") &&
                 ok;
        } else {
            ok = expect(caret.rect.x == 0.0f && caret.rect.y > 0.0f,
                        "caret after a newline sits at the start of the next line") &&
                 ok;
        }
    }
    return ok;
}

bool test_width_colour_and_opacity() {
    Node node(1);
    node.set_bounds({0, 0, 80, 40});
    StubStyles styles;
    styles.set(1, styled({{"font-size", "16px"}, {"color", "white"}, {"opacity", "0.5"}}));
    PaintData data = text_payload("abcdef");
    data.decoration = decoration(1, 2, 4);
    node.set_paint_data(std::move(data));

    RecordingSink one;
    paint_scene(node, styles, one);
    PaintContext scaled;
    scaled.device_scale = 2.0f;
    RecordingSink two;
    paint_scene(node, styles, two, scaled);

    bool ok = expect(one.events[0].color.a == 0.35f * 0.5f, "selection alpha folds node opacity");
    ok = expect(one.events.back().color.a == 0.5f, "caret alpha folds node opacity") && ok;
    ok = expect(one.events.back().rect.width == 1.0f, "caret is 1 px at device scale 1") && ok;
    ok = expect(two.events.back().rect.width == 2.0f, "caret is 2 px at device scale 2") && ok;
    return ok;
}

bool test_clip_interaction() {
    Node root(1);
    root.set_bounds({0, 0, 100, 40});
    auto child = std::make_unique<Node>(2);
    Node& text_node = root.add_child(std::move(child));
    text_node.set_bounds({4, 4, 60, 20});
    PaintData data = text_payload("abcdef");
    data.decoration = decoration(2, 1, 4);
    text_node.set_paint_data(std::move(data));

    StubStyles styles;
    styles.set(1, styled({{"overflow-x", "hidden"}, {"overflow-y", "hidden"}}));
    styles.set(2, decoration_style());
    RecordingSink sink;
    paint_scene(root, styles, sink);

    bool ok = true;
    for (const Event& event : sink.events) {
        ok = expect(event.active_clips == 1,
                    "selection/caret/text all see the parent overflow clip") &&
             ok;
    }
    return ok;
}

bool test_damage_replay() {
    Node root(1);
    root.set_bounds({0, 0, 64, 120});
    PaintData data = text_payload("one two three four five six", PaintWrap::Wrap);
    data.decoration = decoration(8, 0, 22);
    root.set_paint_data(std::move(data));
    StubStyles styles;
    styles.set(1, decoration_style());
    root.clear_dirty(Dirty::All);
    root.mark_dirty(Dirty::Paint);

    ManualClock clock;
    RuntimeLoop loop(clock);
    AnimationRuntime animations;
    DamageAccumulator damage(DamageConfig{Extent2D{400, 300}, 8});
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipeline pipeline(animations, loop, tracker, damage, dirty);
    pipeline.tick(root, 0.016);
    const auto render = pipeline.finish_frame(root);
    if (!expect(render.should_render, "decorated damage frame is due")) {
        return false;
    }
    RecordingSink full;
    paint_scene(root, styles, full);
    RecordingSink clipped;
    const FramePaint painted = pipeline.paint_frame(root, styles, clipped, render.redraw_plan);
    return expect(painted.painted, "damage-clipped decorated paint runs") &&
           expect(clipped.count(EventKind::Fill) == full.count(EventKind::Fill) &&
                      clipped.count(EventKind::Text) == full.count(EventKind::Text),
                  "damage replay emits the same decoration and text counts") &&
           expect(clipped.clip_pops == clipped.clip_pushes, "damage clips are balanced");
}

bool test_empty_text() {
    Node node(1);
    node.set_bounds({0, 0, 40, 20});
    StubStyles styles;
    styles.set(1, decoration_style());
    PaintData data = text_payload("");
    data.decoration = decoration(0, 0, 0);
    node.set_paint_data(std::move(data));
    RecordingSink sink;
    paint_scene(node, styles, sink);
    return expect(sink.events.empty(), "empty text paints no decoration or glyphs");
}

} // namespace

int main() {
    bool ok = true;
    ok = test_no_wrap_caret_positions() && ok;
    ok = test_selection_rect_and_z_order() && ok;
    ok = test_collapsed_and_blink() && ok;
    ok = test_multi_line_selection() && ok;
    ok = test_newline_and_end_mapping() && ok;
    ok = test_width_colour_and_opacity() && ok;
    ok = test_clip_interaction() && ok;
    ok = test_damage_replay() && ok;
    ok = test_empty_text() && ok;
    if (!ok) {
        std::cerr << "paint_text_decoration: failed\n";
        return 1;
    }
    std::cout << "paint_text_decoration: ok\n";
    return 0;
}
