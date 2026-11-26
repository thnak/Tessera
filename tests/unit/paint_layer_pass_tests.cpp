// SPDX-License-Identifier: MIT
//
// The isolated-layer producer in the paint pass (issue #227, offscreen-layers.md section 5.2): with
// `PaintContext::isolate_group_opacity` on and a sink that supports layers, a faded node with
// children paints as one layer (push, its box and children, pop) and its own opacity moves from the
// primitives to the layer; every other case keeps the non-isolated rule. The per-node cache replays
// the same stream, and the layer flags are part of its key.

#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::Dirty;
using tessera::ui::Node;
using tessera::ui::NodeId;
using namespace tessera::ui::paint;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_layer_pass: " << message << "\n";
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

ComputedStyle styled(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle style;
    for (const auto& [property, value] : values) {
        style.set(property, value);
    }
    return style;
}

void clear_tree(Node& node) {
    node.clear_dirty(Dirty::All);
    for (const auto& child : node.children()) {
        clear_tree(*child);
    }
}

// Logs "push(opacity)", "fill(alpha)" and "pop"; supports layers only when asked.
class LogSink final : public PaintSink {
public:
    explicit LogSink(bool layers)
        : layers_(layers) {}
    void fill_rect(const Rect&, const Color& color) override {
        log.push_back("fill(" + std::to_string(std::lround(color.a * 100.0f)) + ")");
    }
    void fill_rounded_rect(const Rect& rect, const Color& color, float) override {
        fill_rect(rect, color);
    }
    [[nodiscard]] bool supports_layers() const override {
        return layers_;
    }
    void push_layer(const LayerDraw& layer) override {
        log.push_back("push(" + std::to_string(std::lround(layer.opacity * 100.0f)) + ")");
    }
    void pop_layer() override {
        log.push_back("pop");
    }
    std::vector<std::string> log;

private:
    bool layers_ = false;
};

// root(1) opaque white; faded(2) opacity 0.5 with an opaque red child(3) and blue child(4).
struct Scene {
    Node root{1};
    StubStyles styles;

    explicit Scene(const char* faded_opacity = "0.5", bool children = true) {
        root.set_bounds({0.0f, 0.0f, 100.0f, 100.0f});
        auto faded = std::make_unique<Node>(2);
        faded->set_bounds({10.0f, 10.0f, 60.0f, 60.0f});
        if (children) {
            auto red = std::make_unique<Node>(3);
            red->set_bounds({0.0f, 0.0f, 30.0f, 30.0f});
            faded->add_child(std::move(red));
            auto blue = std::make_unique<Node>(4);
            blue->set_bounds({20.0f, 20.0f, 30.0f, 30.0f});
            faded->add_child(std::move(blue));
        }
        root.add_child(std::move(faded));
        styles.set(1, styled({{"background-color", "#ffffff"}}));
        styles.set(2, styled({{"background-color", "#00ff00"}, {"opacity", faded_opacity}}));
        styles.set(3, styled({{"background-color", "#ff0000"}}));
        styles.set(4, styled({{"background-color", "#0000ff"}}));
        clear_tree(root);
    }
};

using Log = std::vector<std::string>;

bool layer_wraps_the_group() {
    Scene scene;
    LogSink sink(true);
    PaintContext context;
    context.isolate_group_opacity = true;
    paint_scene(scene.root, scene.styles, sink, context);
    // root fill, then push(50), the faded node's own box and both children at full alpha, pop.
    const Log want{"fill(100)", "push(50)", "fill(100)", "fill(100)", "fill(100)", "pop"};
    if (!expect(sink.log == want, "a faded group paints as one layer at the node's opacity")) {
        for (const auto& entry : sink.log) {
            std::cerr << "  " << entry;
        }
        std::cerr << "\n";
        return false;
    }
    return true;
}

bool default_off_is_flat() {
    Scene scene;
    LogSink sink(true);
    paint_scene(scene.root, scene.styles, sink); // isolate_group_opacity defaults to false
    const Log want{"fill(100)", "fill(50)", "fill(50)", "fill(50)"};
    return expect(sink.log == want, "with the flag off the pass is byte-identical to before");
}

bool unsupported_sink_is_flat() {
    Scene scene;
    LogSink sink(false);
    PaintContext context;
    context.isolate_group_opacity = true;
    paint_scene(scene.root, scene.styles, sink, context);
    const Log want{"fill(100)", "fill(50)", "fill(50)", "fill(50)"};
    return expect(sink.log == want, "a sink without layers keeps the non-isolated rule");
}

bool no_layer_without_children_or_opacity() {
    bool ok = true;
    {
        Scene scene("0.5", false);
        LogSink sink(true);
        PaintContext context;
        context.isolate_group_opacity = true;
        paint_scene(scene.root, scene.styles, sink, context);
        ok = expect((sink.log == Log{"fill(100)", "fill(50)"}), "a leaf is not a group") && ok;
    }
    {
        Scene scene("1");
        LogSink sink(true);
        PaintContext context;
        context.isolate_group_opacity = true;
        paint_scene(scene.root, scene.styles, sink, context);
        ok = expect(sink.log.size() == 4 && sink.log[1] != "push(100)",
                    "opacity 1 opens no layer") &&
             ok;
    }
    {
        Scene scene("0");
        LogSink sink(true);
        PaintContext context;
        context.isolate_group_opacity = true;
        paint_scene(scene.root, scene.styles, sink, context);
        bool pushed = false;
        for (const auto& entry : sink.log) {
            pushed = pushed || entry.rfind("push", 0) == 0;
        }
        ok = expect(!pushed, "opacity 0 stays on the flat path") && ok;
    }
    return ok;
}

bool nested_layers_pair() {
    Scene scene;
    // The red child fades too: a layer inside a layer, the inner opacity applied at its own pop.
    scene.styles.set(3, styled({{"background-color", "#ff0000"}, {"opacity", "0.5"}}));
    auto inner = std::make_unique<Node>(5);
    inner->set_bounds({1.0f, 1.0f, 5.0f, 5.0f});
    scene.root.children()[0]->children()[0]->add_child(std::move(inner));
    scene.styles.set(5, styled({{"background-color", "#ffff00"}}));
    clear_tree(scene.root);
    LogSink sink(true);
    PaintContext context;
    context.isolate_group_opacity = true;
    paint_scene(scene.root, scene.styles, sink, context);
    const Log want{"fill(100)", "push(50)", "fill(100)", "push(50)", "fill(100)",
                   "fill(100)", "pop",      "fill(100)", "pop"};
    return expect(sink.log == want, "nested groups push and pop in order");
}

bool cache_replays_the_same_stream() {
    Scene scene;
    PaintCache cache;
    PaintContext context;
    context.isolate_group_opacity = true;
    context.paint_cache = &cache;
    LogSink first(true);
    paint_scene(scene.root, scene.styles, first, context);
    clear_tree(scene.root);
    LogSink second(true);
    paint_scene(scene.root, scene.styles, second, context);
    LogSink third(true);
    paint_scene(scene.root, scene.styles, third, context);
    return expect(second.log == first.log && third.log == first.log,
                  "a cache hit replays the layer stream, including the pop after the children") &&
           expect(cache.stats().hits > 0, "the second paint hit the cache");
}

bool flag_is_part_of_the_cache_key() {
    Scene scene;
    PaintCache cache;
    PaintContext context;
    context.paint_cache = &cache;
    LogSink flat(true);
    paint_scene(scene.root, scene.styles, flat, context);
    clear_tree(scene.root);
    context.isolate_group_opacity = true;
    LogSink layered(true);
    paint_scene(scene.root, scene.styles, layered, context);
    const Log want{"fill(100)", "push(50)", "fill(100)", "fill(100)", "fill(100)", "pop"};
    return expect(layered.log == want, "turning the flag on does not replay a flat record");
}

} // namespace

int main() {
    const bool ok = layer_wraps_the_group() && default_off_is_flat() &&
                    unsupported_sink_is_flat() && no_layer_without_children_or_opacity() &&
                    nested_layers_pair() && cache_replays_the_same_stream() &&
                    flag_is_part_of_the_cache_key();
    if (!ok) {
        return 1;
    }
    std::cout << "paint_layer_pass: ok\n";
    return 0;
}
