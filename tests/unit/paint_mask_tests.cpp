// SPDX-License-Identifier: MIT
//
// `mask-image` in the paint pass (issue #617, offscreen-layers.md section 5.2). A node with a
// gradient mask paints as one layer: its box and children, then begin_layer_mask, the mask image's
// gradient fills at the node's border box, end_layer_mask, pop. Every degraded case stays balanced:
//  - `none`, a value outside the gradient grammar and a sink without layers paint exactly as
//  before;
//  - a sink that refuses the mask paints the layer without the mask draws;
//  - a masked node is never served from a stale per-node record, and its mask is replayed from a
//    subtree draw list on a sink that can mask and skipped on one that cannot.

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
        std::cerr << "paint_mask: " << message << "\n";
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

// Logs fills, layer push/pop and the mask bracket; `grad(x,y,w,h)` is a mask gradient's box.
class LogSink final : public PaintSink {
public:
    LogSink(bool layers, bool mask)
        : layers_(layers),
          mask_(mask) {}
    void fill_rect(const Rect&, const Color& color) override {
        log.push_back("fill(" + std::to_string(std::lround(color.a * 100.0f)) + ")");
    }
    void fill_rounded_rect(const Rect& rect, const Color& color, float) override {
        fill_rect(rect, color);
    }
    void fill_linear_gradient(const GradientDraw& draw) override {
        log.push_back("grad(" + std::to_string(std::lround(draw.rect.x)) + "," +
                      std::to_string(std::lround(draw.rect.y)) + "," +
                      std::to_string(std::lround(draw.rect.width)) + "," +
                      std::to_string(std::lround(draw.rect.height)) + ")");
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
    [[nodiscard]] bool begin_layer_mask() override {
        log.push_back(mask_ ? "mask{" : "mask?");
        return mask_;
    }
    void end_layer_mask() override {
        log.push_back("}");
    }
    std::vector<std::string> log;

private:
    bool layers_ = false;
    bool mask_ = false;
};

using Log = std::vector<std::string>;

constexpr const char* kMask = "linear-gradient(90deg, #000000, #ffffff)";

// root(1) opaque white; node(2) green at (10,10) 60x60 with the given mask-image and opacity and
// one red child(3).
struct Scene {
    Node root{1};
    StubStyles styles;

    explicit Scene(const char* mask = kMask, const char* opacity = "1") {
        root.set_bounds({0.0f, 0.0f, 100.0f, 100.0f});
        auto node = std::make_unique<Node>(2);
        node->set_bounds({10.0f, 10.0f, 60.0f, 60.0f});
        auto child = std::make_unique<Node>(3);
        child->set_bounds({0.0f, 0.0f, 30.0f, 30.0f});
        node->add_child(std::move(child));
        root.add_child(std::move(node));
        styles.set(1, styled({{"background-color", "#ffffff"}}));
        restyle(mask, opacity);
        styles.set(3, styled({{"background-color", "#ff0000"}}));
        clear_tree(root);
    }
    void restyle(const char* mask, const char* opacity = "1") {
        styles.set(
            2,
            styled({{"background-color", "#00ff00"}, {"opacity", opacity}, {"mask-image", mask}}));
    }
};

bool dump(const Log& log) {
    for (const auto& entry : log) {
        std::cerr << "  " << entry;
    }
    std::cerr << "\n";
    return false;
}

bool mask_wraps_the_group() {
    Scene scene;
    LogSink sink(true, true);
    paint_scene(scene.root, scene.styles, sink);
    // The mask covers the node's border box in its own frame, after the children, inside the layer.
    const Log want{"fill(100)", "push(100)",         "fill(100)", "fill(100)",
                   "mask{",     "grad(10,10,60,60)", "}",         "pop"};
    return sink.log == want ||
           (expect(false, "a masked node paints as one layer with its mask"), dump(sink.log));
}

bool mask_composes_with_opacity() {
    Scene scene(kMask, "0.5");
    LogSink sink(true, true);
    paint_scene(scene.root, scene.styles, sink);
    return expect(sink.log.size() == 8 && sink.log[1] == "push(50)" && sink.log[3] == "fill(100)",
                  "the node's opacity moves to the layer and the content keeps full alpha");
}

bool no_mask_no_layer() {
    bool ok = true;
    for (const char* value : {"none", "url(a.png)", "linear-gradient(", ""}) {
        Scene scene(value);
        LogSink sink(true, true);
        paint_scene(scene.root, scene.styles, sink);
        const Log want{"fill(100)", "fill(100)", "fill(100)"};
        ok = expect(sink.log == want, "a mask-image outside the grammar paints no layer") && ok;
    }
    return ok;
}

bool sink_without_layers_is_unmasked() {
    Scene scene;
    LogSink sink(false, true);
    paint_scene(scene.root, scene.styles, sink);
    const Log want{"fill(100)", "fill(100)", "fill(100)"};
    return expect(sink.log == want, "a sink without layers paints the node unmasked");
}

bool refused_mask_skips_the_draws() {
    Scene scene;
    LogSink sink(true, false);
    paint_scene(scene.root, scene.styles, sink);
    const Log want{"fill(100)", "push(100)", "fill(100)", "fill(100)", "mask?", "pop"};
    return sink.log == want ||
           (expect(false, "a refused mask draws nothing and stays balanced"), dump(sink.log));
}

bool never_served_from_a_stale_record() {
    // Frame 1 has no mask and fills the per-node cache; the mask then appears on a dirty node.
    Scene scene("none");
    PaintCache cache;
    PaintContext context;
    context.paint_cache = &cache;
    LogSink first(true, true);
    paint_scene(scene.root, scene.styles, first, context);
    clear_tree(scene.root);

    scene.restyle(kMask);
    scene.root.children()[0]->mark_dirty(Dirty::Style);
    LogSink second(true, true);
    paint_scene(scene.root, scene.styles, second, context);
    clear_tree(scene.root);
    // A clean frame after it must still carry the mask: the pre-mask entry was dropped.
    LogSink third(true, true);
    paint_scene(scene.root, scene.styles, third, context);
    LogSink fourth(true, true);
    paint_scene(scene.root, scene.styles, fourth, context);

    const Log want{"fill(100)", "push(100)",         "fill(100)", "fill(100)",
                   "mask{",     "grad(10,10,60,60)", "}",         "pop"};
    return expect(second.log == want, "the dirty node paints its new mask") &&
           expect(third.log == want, "the next clean frame still masks, not a stale record") &&
           expect(fourth.log == want, "and so does the one after");
}

// A subtree draw list records the mask as ops and replays it only on a sink that agrees to mask.
bool subtree_list_replays_the_mask() {
    LogSink live(true, true);
    PaintCache::Recorder recorder(live, true);
    recorder.push_layer(LayerDraw{{0.0f, 0.0f, 10.0f, 10.0f}, 1.0f});
    recorder.fill_rect({0.0f, 0.0f, 10.0f, 10.0f}, Color{1.0f, 0.0f, 0.0f, 1.0f});
    const bool opened = recorder.begin_layer_mask();
    GradientDraw gradient;
    gradient.rect = {1.0f, 2.0f, 3.0f, 4.0f};
    recorder.fill_linear_gradient(gradient);
    recorder.end_layer_mask();
    recorder.pop_layer();
    const PaintRecord record = recorder.finish();
    const Log want{"push(100)", "fill(100)", "mask{", "grad(1,2,3,4)", "}", "pop"};
    bool ok = expect(opened, "a subtree recorder always lets the pass emit the mask") &&
              expect(live.log == want, "the live target saw the mask once");

    LogSink capable(true, true);
    replay_paint_ops(record.ops, record.gradients, capable);
    ok = expect(capable.log == want, "a capable sink replays the recorded mask") && ok;

    LogSink refusing(true, false);
    replay_paint_ops(record.ops, record.gradients, refusing);
    const Log skipped{"push(100)", "fill(100)", "mask?", "pop"};
    ok = expect(refusing.log == skipped,
                "a refusing sink skips the mask draws and stays balanced") &&
         ok;
    return ok;
}

bool recorder_gates_a_refused_mask() {
    LogSink refusing(true, false);
    PaintCache::Recorder recorder(refusing, true);
    recorder.push_layer(LayerDraw{{0.0f, 0.0f, 10.0f, 10.0f}, 1.0f});
    (void)recorder.begin_layer_mask();
    GradientDraw gradient;
    recorder.fill_linear_gradient(gradient);
    recorder.end_layer_mask();
    recorder.pop_layer();
    const PaintRecord record = recorder.finish();
    // The target never saw the gradient, but the record kept it for a later capable sink.
    const Log forwarded{"push(100)", "mask?", "pop"};
    LogSink capable(true, true);
    replay_paint_ops(record.ops, record.gradients, capable);
    return expect(refusing.log == forwarded, "a refused mask forwards no mask draws") &&
           expect(capable.log.size() == 5 && capable.log[2].rfind("grad", 0) == 0,
                  "but the record still holds them");
}

} // namespace

int main() {
    const bool ok = mask_wraps_the_group() && mask_composes_with_opacity() && no_mask_no_layer() &&
                    sink_without_layers_is_unmasked() && refused_mask_skips_the_draws() &&
                    never_served_from_a_stale_record() && subtree_list_replays_the_mask() &&
                    recorder_gates_a_refused_mask();
    if (!ok) {
        return 1;
    }
    std::cout << "paint_mask: ok\n";
    return 0;
}
