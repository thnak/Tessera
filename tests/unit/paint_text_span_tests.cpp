// SPDX-License-Identifier: MIT
//
// Inline-span paint (issue #229): one measured paragraph whose runs are split at span boundaries
// with each span's weight/italic/colour, wrapping across spans, the mixed-weight/colour one-line
// golden transcript, and the painted run rectangles that let the generic hit test make the span the
// click target.

#include "tessera/ui/input.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::EventType;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::PaintSpan;
using tessera::ui::PaintTextAlign;
using tessera::ui::PaintWrap;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::RoundedClip;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::TextDraw;
using tessera::ui::style::ComputedStyle;

int failures = 0;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
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
    int font_weight = 0;
    bool italic = false;
    Color color;
    std::optional<float> baseline;
};

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect&, const Color&) override {}
    void fill_rounded_rect(const Rect&, const Color&, float) override {}
    void push_clip(const RoundedClip&) override {}
    void pop_clip() override {}
    void draw_text(const TextDraw& run) override {
        texts.push_back(RecordedText{run.rect, std::string(run.text), run.font_weight, run.italic,
                                     run.color, run.baseline});
    }
    std::vector<RecordedText> texts;
};

struct Scene {
    std::unique_ptr<Node> root;
    Node* text = nullptr;
    Node* first_span = nullptr;
    Node* second_span = nullptr;
    StubStyles styles;
};

// Root View (300x100) with a 280x20 Text at (10,20) whose payload carries two spans. The spans are
// child scene nodes; only the parent gets a layout box (the spans start with bounds 0).
Scene make_scene(bool link_role = false) {
    Scene scene;
    scene.root = std::make_unique<Node>(1);
    auto text = std::make_unique<Node>(2);
    scene.text = text.get();
    scene.text->set_bounds({10, 20, 280, 20});
    PaintData payload;
    payload.kind = tessera::ui::PaintKind::Text;
    payload.text = "By continuing you agree to the Terms";
    payload.spans = {PaintSpan{3, 0, 31}, PaintSpan{4, 31, 36}};
    scene.text->set_paint_data(std::move(payload));
    auto first = std::make_unique<Node>(3);
    scene.first_span = first.get();
    first->set_element_kind("Text");
    auto second = std::make_unique<Node>(4);
    scene.second_span = second.get();
    second->set_element_kind("Text");
    if (link_role) {
        second->set_role("link");
    }
    scene.text->add_child(std::move(first));
    scene.text->add_child(std::move(second));
    scene.root->add_child(std::move(text));
    scene.root->set_bounds({0, 0, 300, 100});
    scene.styles.set(1, styled({{"width", "300"}, {"height", "100"}}));
    scene.styles.set(2,
                     styled({{"font-size", "16"}, {"font-weight", "400"}, {"color", "#000000"}}));
    scene.styles.set(3, styled({{"font-size", "16"}, {"color", "#000000"}}));
    scene.styles.set(4,
                     styled({{"font-size", "16"}, {"font-weight", "700"}, {"color", "#0000ff"}}));
    return scene;
}

// Stub metrics advance = font_size / 2 per byte, so 16 px text advances 8 px per byte.
constexpr float kAdvance = 8.0f;

void test_one_line_golden() {
    Scene scene = make_scene();
    RecordingSink sink;
    paint_scene(*scene.root, scene.styles, sink, PaintContext{});
    // The golden: two runs, the link fragment in bold blue, starting where the plain run ends.
    std::string transcript;
    for (const RecordedText& run : sink.texts) {
        transcript += run.text + "@" + std::to_string(static_cast<int>(std::lround(run.rect.x))) +
                      "+" + std::to_string(static_cast<int>(std::lround(run.rect.width))) + "/" +
                      std::to_string(run.font_weight) + "/" +
                      std::to_string(static_cast<int>(std::lround(run.color.b * 255.0f))) + ";";
    }
    expect(transcript == "By continuing you agree to the @10+248/400/0;Terms@258+40/700/255;",
           "mixed weight and color golden: " + transcript);
    expect(sink.texts.size() == 2, "one line emits one run per span");
    if (sink.texts.size() == 2) {
        expect(sink.texts[0].rect.width == 31 * kAdvance, "the plain run advance");
        expect(sink.texts[1].rect.x == 10.0f + 31 * kAdvance,
               "the link run starts at the boundary");
        expect(!sink.texts[0].italic && !sink.texts[1].italic, "upright runs by default");
    }
}

void test_italic_and_wrap() {
    Scene scene = make_scene();
    // Wrap the paragraph in a 80 px content box: 10 bytes per line (word wrap at spaces).
    scene.text->set_bounds({10, 20, 80, 80});
    PaintData payload = *scene.text->paint_data();
    payload.wrap = PaintWrap::Wrap;
    payload.text = "one two three four";
    payload.spans = {PaintSpan{3, 0, 3}, PaintSpan{4, 4, 7}};
    scene.text->set_paint_data(std::move(payload));
    scene.styles.set(3,
                     styled({{"font-size", "16"}, {"font-style", "italic"}, {"color", "#ff0000"}}));
    scene.styles.set(4,
                     styled({{"font-size", "16"}, {"font-weight", "700"}, {"color", "#0000ff"}}));
    RecordingSink sink;
    paint_scene(*scene.root, scene.styles, sink, PaintContext{});
    std::size_t characters = 0;
    int lines = 0;
    float last_baseline = -1.0f;
    bool saw_bold_two = false;
    bool saw_italic_one = false;
    for (const RecordedText& run : sink.texts) {
        characters += run.text.size();
        if (run.baseline && *run.baseline != last_baseline) {
            last_baseline = *run.baseline;
            ++lines;
        }
        if (run.text == "two" && run.font_weight == 700 && run.color.b > 0.5f) {
            saw_bold_two = true;
        }
        if (run.text == "one" && run.italic && run.color.r > 0.5f) {
            saw_italic_one = true;
        }
    }
    expect(characters >= 15 && characters <= 18,
           "the 15 word characters are painted across the wrapped lines (painted " +
               std::to_string(characters) + ")");
    expect(lines >= 2, "the narrow box wraps the paragraph");
    expect(saw_bold_two, "the bold span keeps its weight on its line");
    expect(saw_italic_one, "the italic span carries the italic flag and colour");
}

void test_hit_test_routes_click_to_span() {
    Scene scene = make_scene(true);
    RecordingSink sink;
    paint_scene(*scene.root, scene.styles, sink, PaintContext{});
    // The painted run rects are parent-relative; the span must be hit-testable at the absolute
    // position of its run (parent at (10,20), link run at x = 10 + 31 * 8 = 258).
    expect(scene.second_span->bounds().width == 40.0f, "the span run rect is stored on the child");
    int span_clicks = 0;
    int text_clicks = 0;
    scene.second_span->bind_event(
        EventType::Click,
        [](void* context, const tessera::ui::Event&) { ++*static_cast<int*>(context); },
        &span_clicks);
    scene.text->bind_event(
        EventType::Click,
        [](void* context, const tessera::ui::Event&) { ++*static_cast<int*>(context); },
        &text_clicks);
    InputRouter router;
    InputEvent down;
    down.type = InputEventType::PointerDown;
    down.x = 270.0f;
    down.y = 28.0f;
    const auto pressed = router.dispatch(*scene.root, down);
    expect(pressed.target == scene.second_span, "pointer down resolves the span under the point");
    InputEvent up = down;
    up.type = InputEventType::PointerUp;
    const auto released = router.dispatch(*scene.root, up);
    expect(released.dispatched, "pointer up dispatches the synthesized click");
    expect(span_clicks == 1, "the span onClick runs");
    expect(text_clicks == 1, "the click bubbles to the parent Text");
    // A point on the plain run stays a parent hit.
    InputEvent plain = down;
    plain.type = InputEventType::PointerDown;
    plain.x = 20.0f;
    const auto plain_pressed = router.dispatch(*scene.root, plain);
    expect(plain_pressed.target == scene.text, "a plain run resolves to the parent Text");
}

void test_transform_skips_span_bounds() {
    Scene scene = make_scene();
    tessera::rendering::rhi::Mat3 transform;
    transform.m[0] = 2.0f;
    scene.text->set_transform(transform);
    RecordingSink sink;
    paint_scene(*scene.root, scene.styles, sink, PaintContext{});
    expect(scene.second_span->bounds().width == 0.0f,
           "a transformed Text keeps parent-level hit testing in V1");
}

} // namespace

int main() {
    test_one_line_golden();
    test_italic_and_wrap();
    test_hit_test_routes_click_to_span();
    test_transform_skips_span_bounds();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
