// SPDX-License-Identifier: MIT
//
// The canvas background: a browser paints the root element's background over the whole canvas and,
// when the root has none, the first child's (`body`) stands in for it. Here the root is the window
// and its first child the app's root view, so the app's page colour reaches every edge of the
// window however short the content is, while the view keeps the box its layout gave it.
//
// Deterministic: no clock, fonts, network or GPU.

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

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
using tessera::ui::paint::canvas_background;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_canvas_background: " << message << "\n";
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

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color& color) override {
        fills.push_back({rect, color});
    }

    void fill_rounded_rect(const Rect& rect, const Color& color, float /*radius*/) override {
        fills.push_back({rect, color});
    }

    std::vector<std::pair<Rect, Color>> fills;
};

// What the host does: resolve the canvas colour once and hand it to the pass.
tessera::ui::paint::PaintContext canvas_context(const Node& root, StyleProvider& styles) {
    tessera::ui::paint::PaintContext context;
    context.canvas = canvas_background(root, styles);
    return context;
}

// A 400x300 window whose app root view is a short 400x120 box at the top.
struct Window {
    Node root{1};
    Node* view = nullptr;

    Window() {
        root.set_bounds({0, 0, 400, 300});
        view = &root.add_child(std::make_unique<Node>(2));
        view->set_bounds({0, 0, 400, 120});
    }
};

bool the_root_views_colour_covers_the_whole_window() {
    Window window;
    StubStyles styles;
    styles.set(2, styled({{"background-color", "#0b1220"}}));

    RecordingSink sink;
    paint_scene(window.root, styles, sink, canvas_context(window.root, styles));

    // The canvas rect first, then the view's own box, which keeps its layout size.
    return expect(sink.fills.size() == 2, "the canvas fill and the view's own fill") &&
           expect(sink.fills[0].first.width == 400.0f && sink.fills[0].first.height == 300.0f,
                  "the canvas covers the whole window") &&
           expect(sink.fills[1].first.height == 120.0f, "the view keeps its laid-out height") &&
           expect(sink.fills[0].second.r == sink.fills[1].second.r &&
                      sink.fills[0].second.b == sink.fills[1].second.b,
                  "the canvas takes the view's colour");
}

bool the_roots_own_background_wins() {
    Window window;
    StubStyles styles;
    styles.set(1, styled({{"background-color", "white"}}));
    styles.set(2, styled({{"background-color", "#0b1220"}}));

    RecordingSink sink;
    paint_scene(window.root, styles, sink, canvas_context(window.root, styles));

    return expect(sink.fills.size() == 2, "the root's box and the view's box, no canvas fill") &&
           expect(!canvas_background(window.root, styles).has_value(),
                  "a root with a background is its own canvas");
}

bool a_translucent_colour_is_not_propagated() {
    Window window;
    StubStyles styles;
    styles.set(2, styled({{"background-color", "rgba(0, 0, 255, 0.5)"}}));

    RecordingSink sink;
    paint_scene(window.root, styles, sink, canvas_context(window.root, styles));

    return expect(sink.fills.size() == 1, "only the view's own box paints") &&
           expect(!canvas_background(window.root, styles).has_value(),
                  "a translucent colour would be blended twice");
}

bool without_a_canvas_the_scene_paints_exactly_what_it_says() {
    Window window;
    StubStyles styles;
    styles.set(2, styled({{"background-color", "#0b1220"}}));

    RecordingSink sink;
    paint_scene(window.root, styles, sink);

    return expect(sink.fills.size() == 1, "the pass adds nothing the caller did not ask for");
}

bool an_unstyled_view_leaves_the_window_to_the_clear_colour() {
    Window window;
    StubStyles styles;

    RecordingSink sink;
    paint_scene(window.root, styles, sink, canvas_context(window.root, styles));

    return expect(sink.fills.empty(), "nothing is painted") &&
           expect(!canvas_background(window.root, styles).has_value(), "no canvas colour");
}

} // namespace

int main() {
    bool ok = true;
    ok &= the_root_views_colour_covers_the_whole_window();
    ok &= the_roots_own_background_wins();
    ok &= a_translucent_colour_is_not_propagated();
    ok &= without_a_canvas_the_scene_paints_exactly_what_it_says();
    ok &= an_unstyled_view_leaves_the_window_to_the_clear_colour();
    return ok ? 0 : 1;
}
