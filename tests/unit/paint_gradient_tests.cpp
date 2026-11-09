// SPDX-License-Identifier: MIT
//
// Paint gradient contract: descriptor normalization (clamp/sort/cap/duplicate), emission order
// (after the background and before borders/content), opacity folding, and the no-gradient path
// staying byte-identical. Structural RecordingSink only; pixel/progress parity lives in the GPU
// test.

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::rendering::rhi::rgba8;
using tessera::ui::LinearGradient;
using tessera::ui::LinearGradientStop;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::paint::GradientDraw;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::RoundedClip;
using tessera::ui::paint::sample_linear_gradient;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::TextDraw;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_gradient: " << message << "\n";
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

ComputedStyle background_style() {
    ComputedStyle style;
    style.set("background-color", "#112233");
    style.set("border-top-left-radius", "8px");
    style.set("border-top-right-radius", "8px");
    style.set("border-bottom-right-radius", "8px");
    style.set("border-bottom-left-radius", "8px");
    style.set("font-size", "16px");
    style.set("color", "#FFFFFF");
    return style;
}

struct Event {
    enum class Kind { Fill, Rounded, Gradient, Text } kind;
    Rect rect;
    GradientDraw gradient;
};

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color&) override {
        events.push_back({Event::Kind::Fill, rect, {}});
    }
    void fill_rounded_rect(const Rect& rect, const Color&, float) override {
        events.push_back({Event::Kind::Rounded, rect, {}});
    }
    void fill_linear_gradient(const GradientDraw& draw) override {
        Event event{Event::Kind::Gradient, draw.rect, draw};
        events.push_back(event);
    }
    void draw_text(const TextDraw& run) override {
        events.push_back({Event::Kind::Text, run.rect, {}});
    }
    std::vector<Event> events;
};

PaintData gradient_payload() {
    PaintData data;
    data.text = "gradient";
    data.gradient = LinearGradient{};
    data.gradient->angle_degrees = 180.0f;
    data.gradient->stop_count = 3;
    data.gradient->stops[0] = LinearGradientStop{0.8f, rgba8(0, 0, 255)};
    data.gradient->stops[1] = LinearGradientStop{-0.5f, rgba8(255, 0, 0)};
    data.gradient->stops[2] = LinearGradientStop{2.0f, rgba8(0, 255, 0)};
    return data;
}

bool test_normalization_order_and_opacity() {
    Node node(1);
    node.set_bounds({0, 0, 100, 80});
    node.set_paint_data(gradient_payload());
    StubStyles styles;
    styles.set(1, background_style());
    RecordingSink sink;
    const auto stats = paint_scene(node, styles, sink);
    return expect(stats.gradients == 1, "one gradient emitted") &&
           expect(sink.events.size() == 3, "background, gradient and text emitted") &&
           expect(sink.events[0].kind != Event::Kind::Gradient, "background first") &&
           expect(sink.events[1].kind == Event::Kind::Gradient, "gradient after background") &&
           expect(sink.events[2].kind == Event::Kind::Text, "text after gradient") &&
           expect(sink.events[1].gradient.stop_count == 3, "all three stops survive") &&
           expect(std::abs(sink.events[1].gradient.stops[0].offset) < 0.001f &&
                      std::abs(sink.events[1].gradient.stops[1].offset - 0.8f) < 0.001f &&
                      std::abs(sink.events[1].gradient.stops[2].offset - 1.0f) < 0.001f,
                  "offsets are clamped and sorted") &&
           expect(sink.events[1].gradient.clip.rounded(), "border radius clips the gradient");
}

bool test_cap_and_opacity() {
    Node node(1);
    node.set_bounds({0, 0, 100, 80});
    PaintData data = gradient_payload();
    data.gradient->stop_count = LinearGradient::kMaxStops + 3;
    for (std::size_t index = 0; index < LinearGradient::kMaxStops; ++index) {
        data.gradient->stops[index] =
            LinearGradientStop{static_cast<float>(index) / 11.0f, rgba8(10, 20, 30, 200)};
    }
    node.set_paint_data(std::move(data));
    StubStyles styles;
    ComputedStyle style = background_style();
    style.set("opacity", "0.5");
    styles.set(1, style);
    RecordingSink sink;
    paint_scene(node, styles, sink);
    const GradientDraw* draw = nullptr;
    for (const Event& event : sink.events) {
        if (event.kind == Event::Kind::Gradient) {
            draw = &event.gradient;
        }
    }
    if (draw == nullptr) {
        return expect(false, "gradient emitted with capped stops");
    }
    const float expected_alpha = (200.0f / 255.0f) * 0.5f;
    return expect(draw->stop_count == LinearGradient::kMaxStops, "stops capped at eight") &&
           expect(std::abs(draw->stops[0].color.a - expected_alpha) < 0.01f,
                  "node opacity folded into stop alpha");
}

bool test_no_gradient_path() {
    Node node(1);
    node.set_bounds({0, 0, 100, 80});
    PaintData data;
    data.text = "plain";
    node.set_paint_data(std::move(data));
    StubStyles styles;
    styles.set(1, background_style());
    RecordingSink sink;
    const auto stats = paint_scene(node, styles, sink);
    return expect(stats.gradients == 0, "no gradient emitted without a descriptor") &&
           expect(sink.events.size() == 2 && sink.events[0].kind != Event::Kind::Gradient &&
                      sink.events[1].kind == Event::Kind::Text,
                  "no-gradient path keeps background + text order");
}

bool test_duplicate_sampling() {
    GradientDraw draw;
    draw.stop_count = 3;
    draw.stops[0] = LinearGradientStop{0.5f, rgba8(255, 0, 0)};
    draw.stops[1] = LinearGradientStop{0.5f, rgba8(0, 255, 0)};
    draw.stops[2] = LinearGradientStop{1.0f, rgba8(0, 0, 255)};
    const Color at_half = sample_linear_gradient(draw, 0.5f);
    return expect(at_half.r == 0.0f && at_half.g == 1.0f,
                  "duplicate offset resolves to the later stop");
}

} // namespace

int main() {
    bool ok = true;
    ok = test_normalization_order_and_opacity() && ok;
    ok = test_cap_and_opacity() && ok;
    ok = test_no_gradient_path() && ok;
    ok = test_duplicate_sampling() && ok;
    if (!ok) {
        std::cerr << "paint_gradient: failed\n";
        return 1;
    }
    std::cout << "paint_gradient: ok\n";
    return 0;
}
