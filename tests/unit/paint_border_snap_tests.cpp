// SPDX-License-Identifier: MIT

// Border widths snap to whole device pixels at paint time (display-aware-rendering.md S3): a 1 CSS
// px border is one crisp device row at 1.25x and 1.5x, a sub-pixel width rises to one device px, a
// fractional device width floors, and scale 1 and 2 keep integral widths unchanged.

#include "tessera/ui/paint/paint_pass.hpp"

#include <cmath>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_border_snap: " << message << "\n";
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

class RectSink final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color& /*color*/) override {
        rects.push_back(rect);
    }
    void fill_rounded_rect(const Rect& rect, const Color& /*color*/, float /*radius*/) override {
        rects.push_back(rect);
    }
    void fill_rounded_ring(const Rect& /*rect*/, const Color& /*color*/, float /*radius*/,
                           float width) override {
        ring_widths.push_back(width);
    }

    std::vector<Rect> rects;
    std::vector<float> ring_widths;
};

ComputedStyle bordered(const char* width, bool rounded) {
    ComputedStyle style;
    for (const char* side : {"top", "right", "bottom", "left"}) {
        const std::string prefix = std::string("border-") + side;
        style.set((prefix + "-width").c_str(), width);
        style.set((prefix + "-style").c_str(), "solid");
        style.set((prefix + "-color").c_str(), "#ff0000");
    }
    if (rounded) {
        style.set("border-top-left-radius", "6px");
        style.set("border-top-right-radius", "6px");
        style.set("border-bottom-right-radius", "6px");
        style.set("border-bottom-left-radius", "6px");
    }
    return style;
}

RectSink paint_box(const char* width, bool rounded, float scale) {
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 40.0f, 20.0f});
    StubStyles styles;
    styles.set(1, bordered(width, rounded));
    RectSink sink;
    PaintContext context;
    context.device_scale = scale;
    paint_scene(root, styles, sink, context);
    return sink;
}

// The painted top strip is the first emitted rect: its height is the top border width.
float top_width(const char* width, float scale) {
    const RectSink sink = paint_box(width, false, scale);
    return sink.rects.empty() ? -1.0f : sink.rects.front().height;
}

bool near(float a, float b) {
    return std::fabs(a - b) < 1e-3f;
}

} // namespace

int main() {
    bool ok = true;

    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        const float device_rows = top_width("1px", scale) * scale;
        ok &= expect(near(device_rows, 1.0f) || near(device_rows, std::floor(device_rows)),
                     "a 1 CSS px border is a whole number of device rows");
        ok &= expect(near(device_rows, std::floor(1.0f * scale + 1e-4f)),
                     "a 1 CSS px border is floor(scale) device rows");
    }
    ok &= expect(near(top_width("1px", 1.0f), 1.0f), "scale 1 keeps a 1px border");
    ok &= expect(near(top_width("1px", 2.0f), 1.0f), "scale 2 keeps a 1px border");
    ok &= expect(near(top_width("1px", 1.25f) * 1.25f, 1.0f), "1px at 1.25 is one device row");
    ok &= expect(near(top_width("1px", 1.5f) * 1.5f, 1.0f), "1px at 1.5 is one device row");

    ok &= expect(near(top_width("0.5px", 1.0f) * 1.0f, 1.0f), "a sub-pixel width rises to 1 px");
    ok &=
        expect(near(top_width("0.25px", 2.0f) * 2.0f, 1.0f), "a sub-pixel width at 2x rises to 1");
    ok &= expect(near(top_width("1.5px", 1.25f) * 1.25f, 1.0f),
                 "1.5px at 1.25 floors from 1.875 to 1 device row");
    ok &= expect(near(top_width("2px", 1.5f) * 1.5f, 3.0f), "2px at 1.5 is exactly 3 rows");

    // A zero width paints nothing, and an unusable scale leaves the width alone.
    ok &= expect(paint_box("0px", false, 1.25f).rects.empty(), "a zero width emits no strip");
    ok &= expect(near(top_width("1px", 0.0f), 1.0f), "scale 0 leaves the width unchanged");

    // A uniform border on a rounded box is a ring and snaps the same way.
    const RectSink ring = paint_box("1px", true, 1.5f);
    ok &= expect(ring.ring_widths.size() == 1 && near(ring.ring_widths.front() * 1.5f, 1.0f),
                 "a rounded ring snaps to whole device rows");

    // Deterministic: the same inputs give the same strips.
    const RectSink first = paint_box("1px", false, 1.25f);
    const RectSink second = paint_box("1px", false, 1.25f);
    ok &= expect(first.rects.size() == second.rects.size() && first.rects.size() == 4,
                 "four strips, repeatable");

    return ok ? 0 : 1;
}
