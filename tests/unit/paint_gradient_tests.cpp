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
using tessera::ui::RadialGradient;
using tessera::ui::paint::ConicBand;
using tessera::ui::paint::ConicGradientDraw;
using tessera::ui::paint::GradientDraw;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::RadialBand;
using tessera::ui::paint::RadialGradientDraw;
using tessera::ui::paint::RoundedClip;
using tessera::ui::paint::sample_linear_gradient;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::tessellate_conic_gradient;
using tessera::ui::paint::tessellate_radial_gradient;
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
    enum class Kind { Fill, Rounded, Gradient, Radial, Conic, Text } kind;
    Rect rect;
    GradientDraw gradient;
    RadialGradientDraw radial;
    ConicGradientDraw conic;
};

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color&) override {
        events.push_back({Event::Kind::Fill, rect, {}, {}, {}});
    }
    void fill_rounded_rect(const Rect& rect, const Color&, float) override {
        events.push_back({Event::Kind::Rounded, rect, {}, {}, {}});
    }
    void fill_linear_gradient(const GradientDraw& draw) override {
        Event event{Event::Kind::Gradient, draw.rect, draw, {}, {}};
        events.push_back(event);
    }
    void fill_radial_gradient(const RadialGradientDraw& draw) override {
        Event event{Event::Kind::Radial, draw.rect, {}, draw, {}};
        events.push_back(event);
    }
    void fill_conic_gradient(const ConicGradientDraw& draw) override {
        Event event;
        event.kind = Event::Kind::Conic;
        event.rect = draw.rect;
        event.conic = draw;
        events.push_back(event);
    }
    void draw_text(const TextDraw& run) override {
        events.push_back({Event::Kind::Text, run.rect, {}, {}, {}});
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

// A radial payload goes through the same clamp/sort/cap/opacity normalization as a linear one, and
// its centre/radii are fractions of the node box rather than frozen pixels.
bool test_radial_payload_normalization() {
    Node node(1);
    node.set_bounds({10, 20, 200, 100});
    PaintData data;
    data.text = "radial";
    data.gradient = LinearGradient{};
    data.gradient->shape = tessera::ui::GradientShape::Radial;
    data.gradient->radial.center_x = 0.25f;
    data.gradient->radial.center_y = 0.75f;
    data.gradient->radial.radius_x = 0.5f;
    data.gradient->radial.radius_y = 0.25f;
    data.gradient->stop_count = 3;
    data.gradient->stops[0] = LinearGradientStop{0.9f, rgba8(0, 0, 255, 200)};
    data.gradient->stops[1] = LinearGradientStop{-1.0f, rgba8(255, 0, 0, 200)};
    data.gradient->stops[2] = LinearGradientStop{2.0f, rgba8(0, 255, 0, 200)};
    node.set_paint_data(std::move(data));
    StubStyles styles;
    ComputedStyle style = background_style();
    style.set("opacity", "0.5");
    styles.set(1, style);
    RecordingSink sink;
    const auto stats = paint_scene(node, styles, sink);
    const RadialGradientDraw* draw = nullptr;
    for (const Event& event : sink.events) {
        if (event.kind == Event::Kind::Radial) {
            draw = &event.radial;
        }
    }
    if (draw == nullptr) {
        return expect(false, "radial gradient emitted");
    }
    const float expected_alpha = (200.0f / 255.0f) * 0.5f;
    return expect(stats.gradients == 1, "one radial gradient counted") &&
           expect(draw->stop_count == 3, "all three radial stops survive") &&
           expect(std::abs(draw->stops[0].offset) < 0.001f &&
                      std::abs(draw->stops[1].offset - 0.9f) < 0.001f &&
                      std::abs(draw->stops[2].offset - 1.0f) < 0.001f,
                  "radial offsets are clamped and sorted") &&
           expect(std::abs(draw->center_x - 60.0f) < 0.001f &&
                      std::abs(draw->center_y - 95.0f) < 0.001f,
                  "the radial centre resolves against the node box") &&
           expect(std::abs(draw->radius_x - 100.0f) < 0.001f &&
                      std::abs(draw->radius_y - 25.0f) < 0.001f,
                  "the radial radii resolve against the node box") &&
           expect(std::abs(draw->stops[0].color.a - expected_alpha) < 0.01f,
                  "node opacity folded into radial stop alpha") &&
           expect(draw->clip.rounded(), "border radius clips the radial gradient");
}

// A radial payload wins over a linear one, because a node paints one background image.
bool test_radial_wins_over_linear() {
    Node node(1);
    node.set_bounds({0, 0, 100, 80});
    PaintData data = gradient_payload();
    data.gradient = LinearGradient{};
    data.gradient->shape = tessera::ui::GradientShape::Radial;
    data.gradient->stop_count = 2;
    data.gradient->stops[0] = LinearGradientStop{0.0f, rgba8(255, 0, 0)};
    data.gradient->stops[1] = LinearGradientStop{1.0f, rgba8(0, 0, 255)};
    node.set_paint_data(std::move(data));
    StubStyles styles;
    styles.set(1, background_style());
    RecordingSink sink;
    paint_scene(node, styles, sink);
    bool radials = false;
    bool linears = false;
    for (const Event& event : sink.events) {
        radials = radials || event.kind == Event::Kind::Radial;
        linears = linears || event.kind == Event::Kind::Gradient;
    }
    return expect(radials && !linears, "the radial payload wins over the linear one");
}

// A conic gradient (issue #617) reaches the sink with its resolved centre and start angle, and the
// stops are clamped and sorted exactly as the other two shapes' are.
bool test_conic_gradient() {
    Node node(1);
    node.set_bounds({10, 20, 200, 100});
    PaintData data;
    data.text = "conic";
    data.gradient = LinearGradient{};
    data.gradient->shape = tessera::ui::GradientShape::Conic;
    data.gradient->radial.center_x = 0.25f;
    data.gradient->radial.center_y = 0.75f;
    data.gradient->stop_count = 3;
    data.gradient->stops[0] = LinearGradientStop{0.8f, rgba8(0, 0, 255)};
    data.gradient->stops[1] = LinearGradientStop{-0.5f, rgba8(255, 0, 0)};
    data.gradient->stops[2] = LinearGradientStop{2.0f, rgba8(0, 255, 0)};
    node.set_paint_data(std::move(data));
    StubStyles styles;
    styles.set(1, background_style());
    RecordingSink sink;
    paint_scene(node, styles, sink);
    const ConicGradientDraw* draw = nullptr;
    for (const Event& event : sink.events) {
        if (event.kind == Event::Kind::Conic) {
            draw = &event.conic;
        }
    }
    if (draw == nullptr) {
        return expect(false, "conic gradient emitted");
    }
    return expect(draw->stop_count == 3, "all three conic stops survive") &&
           expect(std::abs(draw->stops[0].offset) < 0.001f &&
                      std::abs(draw->stops[1].offset - 0.8f) < 0.001f &&
                      std::abs(draw->stops[2].offset - 1.0f) < 0.001f,
                  "conic offsets are clamped and sorted") &&
           expect(std::abs(draw->center_x - 60.0f) < 0.001f &&
                      std::abs(draw->center_y - 95.0f) < 0.001f,
                  "the conic centre resolves against the node box") &&
           expect(draw->clip.rounded(), "border radius clips the conic gradient");
}

// The radial and conic cells tile the box exactly (issue #617, the Chromium comparison of
// `mask-image`): a cell grown past its neighbour composited a translucent stop twice. On a
// fractional box every cell row must run edge to edge with no gap and no overlap, and the cells
// must add up to the box, for the grid the radial shape fills when its ellipse covers the box and
// for the conic one, which always fills it.
template<typename Band>
bool tiles_without_overlap(const std::vector<Band>& bands, const Rect& box, const char* what) {
    constexpr float kEps = 1.0e-3f;
    double area = 0.0;
    bool ok = expect(!bands.empty(), what);
    for (std::size_t i = 0; i < bands.size(); ++i) {
        area += static_cast<double>(bands[i].rect.width) * bands[i].rect.height;
        if (i + 1 < bands.size() && bands[i + 1].rect.y == bands[i].rect.y) {
            const float gap = bands[i + 1].rect.x - (bands[i].rect.x + bands[i].rect.width);
            if (std::abs(gap) > kEps) {
                std::cerr << "paint_gradient: " << what << " cells " << i << " and " << i + 1
                          << " are " << gap << " apart\n";
                ok = false;
            }
        }
    }
    const double box_area = static_cast<double>(box.width) * box.height;
    ok = expect(std::abs(area - box_area) < 0.01, "the cells add up to the box area") && ok;
    const Band& first = bands.front();
    const Band& last = bands.back();
    ok = expect(std::abs(first.rect.x - box.x) < kEps && std::abs(first.rect.y - box.y) < kEps,
                "the first cell starts at the box corner") &&
         ok;
    ok = expect(std::abs(last.rect.x + last.rect.width - (box.x + box.width)) < kEps &&
                    std::abs(last.rect.y + last.rect.height - (box.y + box.height)) < kEps,
                "the last cell ends at the opposite corner") &&
         ok;
    return ok;
}

bool test_cells_tile_without_overlap() {
    const Rect box{8.3f, 7.9f, 47.7f, 48.1f};
    bool ok = true;
    {
        RadialGradientDraw draw;
        draw.rect = box;
        draw.center_x = box.x + box.width * 0.5f;
        draw.center_y = box.y + box.height * 0.5f;
        draw.radius_x = 1000.0f; // the ellipse covers the whole box, so no cell is skipped
        draw.radius_y = 1000.0f;
        draw.stop_count = 2;
        draw.stops[0] = {0.0f, {0.0f, 0.0f, 0.0f, 0.0f}};
        draw.stops[1] = {1.0f, {0.0f, 0.0f, 0.0f, 1.0f}};
        std::vector<RadialBand> bands;
        tessellate_radial_gradient(draw, [&](const RadialBand& band) { bands.push_back(band); });
        ok = tiles_without_overlap(bands, box, "radial cells") && ok;
    }
    {
        ConicGradientDraw draw;
        draw.rect = box;
        draw.center_x = box.x + box.width * 0.5f;
        draw.center_y = box.y + box.height * 0.5f;
        draw.stop_count = 2;
        draw.stops[0] = {0.0f, {0.0f, 0.0f, 0.0f, 0.0f}};
        draw.stops[1] = {1.0f, {0.0f, 0.0f, 0.0f, 1.0f}};
        std::vector<ConicBand> bands;
        tessellate_conic_gradient(draw, [&](const ConicBand& band) { bands.push_back(band); });
        ok = tiles_without_overlap(bands, box, "conic cells") && ok;
    }
    return ok;
}

} // namespace
int main() {
    bool ok = true;
    ok = test_cells_tile_without_overlap() && ok;
    ok = test_normalization_order_and_opacity() && ok;
    ok = test_cap_and_opacity() && ok;
    ok = test_no_gradient_path() && ok;
    ok = test_duplicate_sampling() && ok;
    ok = test_radial_payload_normalization() && ok;
    ok = test_radial_wins_over_linear() && ok;
    ok = test_conic_gradient() && ok;
    if (!ok) {
        std::cerr << "paint_gradient: failed\n";
        return 1;
    }
    std::cout << "paint_gradient: ok\n";
    return 0;
}
