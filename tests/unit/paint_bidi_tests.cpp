// SPDX-License-Identifier: MIT
//
// Paint bidi emission: each line's visual runs are drawn in visual order with correct x, LTR
// no-visual-run output is byte-identical to the pre-bidi path, and caret/selection map logical
// offsets through the right-biased bidi affinity. Structural (RecordingSink) with stub metrics so
// no font is needed; pixel proof lives in paint_bidi_gpu.

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/text/text_measure.hpp"

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

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::PaintWrap;
using tessera::ui::TextDecoration;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::RoundedClip;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::TextDraw;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_bidi: " << message << "\n";
    }
    return condition;
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

ComputedStyle style(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle result;
    for (const auto& [property, value] : values) {
        result.set(property, value);
    }
    return result;
}

struct DrawnRun {
    std::string text;
    Rect rect;
};

struct Fill {
    Rect rect;
    Color color;
};

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color& color) override {
        fills.push_back(Fill{rect, color});
    }
    void fill_rounded_rect(const Rect& rect, const Color& color, float) override {
        fill_rect(rect, color);
    }
    void push_clip(const RoundedClip&) override {}
    void pop_clip() override {}
    void draw_text(const TextDraw& run) override {
        runs.push_back(DrawnRun{std::string(run.text), run.rect});
    }
    std::vector<DrawnRun> runs;
    std::vector<Fill> fills;
};

PaintData payload(std::string text, PaintWrap wrap = PaintWrap::NoWrap) {
    PaintData data;
    data.text = std::move(text);
    data.wrap = wrap;
    data.text_style.font_size = 16.0f;
    data.text_style.color = tessera::rendering::rhi::rgba8(255, 255, 255);
    return data;
}

bool test_ltr_non_regression() {
    Node node(1);
    node.set_bounds({0, 0, 100, 40});
    node.set_paint_data(payload("abc def"));
    StubStyles styles;
    styles.set(1, style({{"font-size", "16px"}, {"color", "white"}}));
    RecordingSink sink;
    paint_scene(node, styles, sink);
    return expect(sink.runs.size() == 1, "pure LTR no-wrap emits one run") &&
           expect(sink.runs[0].rect.x == 0.0f && sink.runs[0].rect.y == 0.0f &&
                      sink.runs[0].rect.width == 100.0f && sink.runs[0].rect.height == 40.0f,
                  "pure LTR no-wrap keeps the content-box rect") &&
           expect(sink.runs[0].text == "abc def", "pure LTR keeps the whole text");
}

bool test_pure_rtl_order_and_caret() {
    const std::string hebrew = "\xD7\x90\xD7\x91\xD7\x92";
    Node node(1);
    node.set_bounds({0, 0, 100, 40});
    PaintData data = payload(hebrew);
    node.set_paint_data(std::move(data));
    StubStyles styles;
    styles.set(1, style({{"font-size", "16px"}, {"color", "white"}}));
    RecordingSink sink;
    paint_scene(node, styles, sink);
    bool ok = expect(sink.runs.size() == 1, "pure RTL emits one visual run") &&
              expect(sink.runs[0].text == hebrew, "pure RTL run covers the whole text");
    if (sink.runs.empty()) {
        return false;
    }
    ok = expect(std::abs(sink.runs[0].rect.width - 24.0f) < 0.01f,
                "RTL run width is the measured advance") &&
         ok;

    PaintData decorated = payload(hebrew);
    TextDecoration decoration;
    decoration.caret_byte_offset = 0; // logical start of an RTL line = visual right edge
    decorated.decoration = decoration;
    node.set_paint_data(std::move(decorated));
    RecordingSink caret_sink;
    paint_scene(node, styles, caret_sink);
    ok = expect(!caret_sink.fills.empty(), "RTL caret emits a fill") && ok;
    if (!caret_sink.fills.empty()) {
        ok = expect(std::abs(caret_sink.fills.back().rect.x - 24.0f) < 0.01f,
                    "RTL caret at logical start maps to the visual right edge") &&
             ok;
    }
    return ok;
}

bool test_mixed_ltr_rtl_runs() {
    const std::string mixed = "abc \xD7\x90\xD7\x91";
    Node node(1);
    node.set_bounds({0, 0, 200, 40});
    node.set_paint_data(payload(mixed));
    StubStyles styles;
    styles.set(1, style({{"font-size", "16px"}, {"color", "white"}}));
    RecordingSink sink;
    paint_scene(node, styles, sink);
    bool ok = expect(sink.runs.size() == 2, "mixed line emits two visual runs") &&
              expect(sink.runs[0].text == "abc ", "LTR run first in visual order") &&
              expect(sink.runs[1].text == "\xD7\x90\xD7\x91", "RTL run second in visual order");
    if (sink.runs.size() == 2) {
        ok = expect(sink.runs[0].rect.x == 0.0f && sink.runs[1].rect.x == 32.0f,
                    "visual run x follows the shaped LTR advance") &&
             ok;
    }

    PaintData decorated = payload(mixed);
    TextDecoration decoration;
    decoration.caret_byte_offset = 4; // join: right-biased affinity -> end of the LTR run
    decoration.selection_begin = 2;
    decoration.selection_end = 6;
    decorated.decoration = decoration;
    node.set_paint_data(std::move(decorated));
    RecordingSink join_sink;
    paint_scene(node, styles, join_sink);
    ok = expect(!join_sink.fills.empty(), "mixed decoration emits fills") && ok;
    if (!join_sink.fills.empty()) {
        const Fill& selection = join_sink.fills.front();
        ok = expect(selection.rect.x == 16.0f && std::abs(selection.rect.width - 16.0f) < 0.01f,
                    "selection inside the LTR run maps to its logical prefix") &&
             ok;
        ok = expect(std::abs(join_sink.fills.back().rect.x - 32.0f) < 0.01f,
                    "caret at the LTR/RTL join uses the right-biased visual x") &&
             ok;
        ok = expect(join_sink.fills.size() >= 2 &&
                        std::abs(join_sink.fills[1].rect.x - 40.0f) < 0.01f &&
                        std::abs(join_sink.fills[1].rect.width - 8.0f) < 0.01f,
                    "selection spanning the join paints the RTL run visual interval") &&
             ok;
    }
    return ok;
}

bool test_determinism_and_wrap() {
    const std::string text = "abc \xD7\x90\xD7\x91 def \xD7\x93\xD7\x94";
    Node node(1);
    node.set_bounds({0, 0, 64, 100});
    PaintData data = payload(text, PaintWrap::Wrap);
    node.set_paint_data(std::move(data));
    StubStyles styles;
    styles.set(1, style({{"font-size", "16px"}, {"color", "white"}}));
    RecordingSink first;
    RecordingSink second;
    paint_scene(node, styles, first);
    paint_scene(node, styles, second);
    bool ok = expect(first.runs.size() == second.runs.size() && first.runs.size() > 1,
                     "wrapped bidi text emits multiple visual runs deterministically");
    for (std::size_t index = 0; index < first.runs.size() && index < second.runs.size(); ++index) {
        ok = expect(first.runs[index].text == second.runs[index].text &&
                        first.runs[index].rect.x == second.runs[index].rect.x &&
                        first.runs[index].rect.y == second.runs[index].rect.y,
                    "wrapped bidi runs are deterministic") &&
             ok;
    }
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = test_ltr_non_regression() && ok;
    ok = test_pure_rtl_order_and_caret() && ok;
    ok = test_mixed_ltr_rtl_runs() && ok;
    ok = test_determinism_and_wrap() && ok;
    if (!ok) {
        std::cerr << "paint_bidi: failed\n";
        return 1;
    }
    std::cout << "paint_bidi: ok\n";
    return 0;
}
