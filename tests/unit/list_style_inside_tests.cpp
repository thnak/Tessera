// SPDX-License-Identifier: MIT

// `list-style-position: inside` (docs/design/list-style-inside.md): the first-line box in the wrap,
// the item-to-first-Text resolver and the marker painted into the box.

#include "tessera/ui/paint/list_marker_inside.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/text_layout.hpp"

#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace style = tessera::ui::style;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::RoundedClip;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::TextDraw;

int failures = 0;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "list_style_inside: " << message << "\n";
        ++failures;
    }
    return condition;
}

class Styles final : public StyleProvider {
public:
    void set(NodeId id, std::initializer_list<std::pair<const char*, const char*>> values) {
        style::ComputedStyle computed;
        for (const auto& [property, value] : values) {
            computed.set(property, value);
        }
        styles_[id] = std::move(computed);
    }
    style::ComputedStyle compute(const Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? style::ComputedStyle{} : found->second;
    }

private:
    std::map<NodeId, style::ComputedStyle> styles_;
};

class Recorder final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color& color) override {
        shapes.emplace_back('s', rect, color);
    }
    void fill_rounded_rect(const Rect& rect, const Color& color, float) override {
        shapes.emplace_back('d', rect, color);
    }
    void fill_rounded_ring(const Rect& rect, const Color& color, float, float) override {
        shapes.emplace_back('c', rect, color);
    }
    void push_clip(const RoundedClip&) override {}
    void pop_clip() override {}
    void draw_text(const TextDraw& run) override {
        texts.emplace_back(std::string(run.text), run.rect);
    }
    std::vector<std::tuple<char, Rect, Color>> shapes;
    std::vector<std::pair<std::string, Rect>> texts;
};

// --- list-style-position: inside (docs/design/list-style-inside.md) ---

// A list item holding a first Text and a second Text: only the first hosts the marker.
struct InsideItem {
    std::unique_ptr<Node> item;
    Styles styles;
    Node* first = nullptr;
    Node* second = nullptr;
};

InsideItem make_inside(const char* type, const char* position = "inside", bool with_text = true) {
    InsideItem result;
    result.item = std::make_unique<Node>(1);
    result.item->set_bounds({40, 0, 160, 40});
    result.item->set_role("listitem");
    result.styles.set(1, {{"font-size", "16"},
                          {"color", "#000000"},
                          {"list-style-type", type},
                          {"list-style-position", position}});
    if (!with_text) {
        return result;
    }
    for (NodeId id = 2; id <= 3; ++id) {
        auto text = std::make_unique<Node>(id);
        text->set_element_kind("Text");
        text->set_bounds({0, static_cast<float>((id - 2) * 20), 160, 20});
        tessera::ui::PaintData data;
        data.kind = tessera::ui::PaintKind::Text;
        data.text = id == 2 ? "first" : "second";
        text->set_paint_data(std::move(data));
        (id == 2 ? result.first : result.second) = text.get();
        result.item->add_child(std::move(text));
        result.styles.set(id, {{"font-size", "16"},
                               {"color", "#000000"},
                               {"list-style-type", type},
                               {"list-style-position", position}});
    }
    return result;
}

void test_inside_marker() {
    {
        InsideItem list = make_inside("disc");
        Recorder sink;
        tessera::ui::paint::paint_scene(*list.item, list.styles, sink, PaintContext{});
        expect(sink.shapes.size() == 1, "one disc for one item");
        const float indent = 16.0f * (0.3125f + 0.55f);
        if (!sink.shapes.empty()) {
            const Rect& disc = std::get<1>(sink.shapes[0]);
            expect(disc.x >= 40.0f && disc.x + disc.width <= 40.0f + indent,
                   "the disc sits inside the first line's box, not before the item");
        }
        expect(sink.texts.size() == 2, "both Texts still paint");
        if (sink.texts.size() == 2) {
            expect(sink.texts[0].second.x >= 40.0f + indent - 1.0f,
                   "the first line's glyphs start after the marker box");
            expect(sink.texts[1].second.x == 40.0f, "only the first Text is indented");
        }
    }
    {
        InsideItem list = make_inside("decimal");
        Recorder sink;
        tessera::ui::paint::paint_scene(*list.item, list.styles, sink, PaintContext{});
        expect(sink.texts.size() == 3, "the counter and both Texts");
        if (sink.texts.size() == 3) {
            expect(sink.texts[0].first == "1. ", "the marker is drawn into the first Text");
            expect(sink.texts[0].second.x == 40.0f, "it starts at the line start");
            expect(sink.texts[1].first == "first", "then the text");
            expect(sink.texts[1].second.x == sink.texts[0].second.x + sink.texts[0].second.width,
                   "right after the marker box");
            expect(sink.texts[2].second.x == 40.0f, "the second Text is not indented");
        }
    }
    {
        // No Text of its own: the item keeps an outside marker rather than losing it.
        InsideItem list = make_inside("disc", "inside", false);
        Recorder sink;
        tessera::ui::paint::paint_scene(*list.item, list.styles, sink, PaintContext{});
        expect(sink.shapes.size() == 1, "an item without a Text falls back to outside");
        if (!sink.shapes.empty()) {
            expect(std::get<1>(sink.shapes[0]).x + std::get<1>(sink.shapes[0]).width <= 40.0f,
                   "and draws it before the item");
        }
    }
    {
        InsideItem list = make_inside("none");
        Recorder sink;
        tessera::ui::paint::paint_scene(*list.item, list.styles, sink, PaintContext{});
        expect(sink.shapes.empty() && sink.texts.size() == 2, "none reserves and draws nothing");
        expect(sink.texts.size() == 2 && sink.texts[0].second.x == 40.0f, "and does not indent");
    }
    {
        InsideItem list = make_inside("decimal", "outside");
        Recorder sink;
        tessera::ui::paint::paint_scene(*list.item, list.styles, sink, PaintContext{});
        expect(sink.texts.size() == 3 && sink.texts[0].first == "1. " &&
                   sink.texts[0].second.x + sink.texts[0].second.width == 40.0f,
               "outside still draws before the item box");
    }
    {
        // Layout and paint resolve the same box.
        InsideItem list = make_inside("lower-roman");
        const auto marker = tessera::ui::paint::inside_marker(
            *list.first, list.styles,
            [](std::string_view run, const tessera::ui::text::TextStyle& style, const void*) {
                return static_cast<float>(run.size()) * style.font_size * 0.5f;
            },
            nullptr);
        expect(marker && marker->width == 3.0f * 8.0f, "the width is the counter's advance");
        expect(!tessera::ui::paint::inside_marker(*list.second, list.styles, nullptr, nullptr),
               "a later Text hosts nothing");
        expect(tessera::ui::paint::marker_host_item(*list.first) == list.item.get(),
               "the host item of the first Text");
        expect(tessera::ui::paint::marker_host_item(*list.second) == nullptr,
               "no host item for a later Text");
    }
}

// The wrap: the first line gives up the box and carries it in its width; later lines do not.
void test_first_line_indent_wrap() {
    namespace text = tessera::ui::text;
    const text::FontMetrics metrics = text::StubFontMetricsProvider{}.metrics(text::TextStyle{});
    text::TextStyle plain;
    plain.font_size = 16.0f;
    text::TextConstraints constraints;
    constraints.max_width = 100.0f;
    const text::TextLayout base =
        text::layout_text("aaaa bbbb cccc dddd", plain, metrics, constraints);
    text::TextStyle indented = plain;
    indented.first_line_indent = 24.0f;
    const text::TextLayout with =
        text::layout_text("aaaa bbbb cccc dddd", indented, metrics, constraints);
    expect(!base.lines.empty() && base.lines[0].indent == 0.0f, "no indent by default");
    expect(with.lines.size() >= base.lines.size(), "the box can only add lines");
    if (!with.lines.empty()) {
        expect(with.lines[0].indent == 24.0f, "the first line records the box");
        expect(with.lines[0].width <= 100.0f, "and the line still fits with it");
        expect(with.lines[0].end <= base.lines[0].end, "a narrower first line holds no more text");
    }
    for (std::size_t i = 1; i < with.lines.size(); ++i) {
        expect(with.lines[i].indent == 0.0f, "later lines have no box");
    }
    const text::TextLayout hard = text::layout_text("ab\ncd", indented, metrics, constraints);
    expect(hard.lines.size() == 2 && hard.lines[0].indent == 24.0f && hard.lines[1].indent == 0.0f,
           "a hard newline starts a line without the box");
    text::TextConstraints one_line;
    one_line.wrap = false;
    const text::TextLayout single = text::layout_text("ab", indented, metrics, one_line);
    const text::TextLayout single_plain = text::layout_text("ab", plain, metrics, one_line);
    expect(single.width == single_plain.width + 24.0f, "the box is part of the line width");
}

} // namespace

int main() {
    test_inside_marker();
    test_first_line_indent_wrap();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
