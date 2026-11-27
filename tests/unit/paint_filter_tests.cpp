// SPDX-License-Identifier: MIT
//
// `filter` in the paint pass (issue #617, offscreen-layers.md section 5.2). A node whose `filter`
// parses to blur() / drop-shadow() paints as one isolated layer whose push carries the chain:
//  - `none`, a value outside the grammar, a no-op blur and a sink without layers paint as before;
//  - sigmas are clamped to the configured glassMaxBlurRadius, `currentcolor` and an omitted shadow
//    colour resolve against the node's text colour, and the node's opacity moves to the layer;
//  - a per-node paint cache keeps painting the filter on clean frames, a subtree draw list records
//    the chain and replays it verbatim;
//  - the style sync widens the node's paint outsets and dependency sigma by the filter's reach, so
//    damage and culling cover the halo.

#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/scene_style_sync.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::gpu::LayerFilterOp;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::Dirty;
using tessera::ui::Node;
using tessera::ui::NodeId;
using namespace tessera::ui::paint;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_filter: " << message << "\n";
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

std::string number(float value) {
    return std::to_string(std::lround(value * 100.0f));
}

// Logs fills and the layer bracket; a push shows the opacity and each filter function as
// `blur:<sigma>` or `shadow:<dx>,<dy>,<sigma>,<r>,<g>,<b>,<a>` (values x100, rounded).
class LogSink final : public PaintSink {
public:
    explicit LogSink(bool layers)
        : layers_(layers) {}
    void fill_rect(const Rect&, const Color& color) override {
        log.push_back("fill(" + number(color.a) + ")");
    }
    void fill_rounded_rect(const Rect& rect, const Color& color, float) override {
        fill_rect(rect, color);
    }
    [[nodiscard]] bool supports_layers() const override {
        return layers_;
    }
    void push_layer(const LayerDraw& layer) override {
        std::string entry = "push(" + number(layer.opacity);
        for (std::size_t index = 0; index < layer.filter.count; ++index) {
            const LayerFilterOp& op = layer.filter.ops[index];
            entry += op.kind == LayerFilterOp::Kind::Blur
                         ? ";blur:" + number(op.sigma)
                         : ";shadow:" + number(op.offset_x) + "," + number(op.offset_y) + "," +
                               number(op.sigma) + "," + number(op.color.r) + "," +
                               number(op.color.g) + "," + number(op.color.b) + "," +
                               number(op.color.a);
        }
        log.push_back(entry + ")");
    }
    void pop_layer() override {
        log.push_back("pop");
    }
    std::vector<std::string> log;

private:
    bool layers_ = false;
};

using Log = std::vector<std::string>;

// root(1) white; node(2) green at (10,10) 60x60 with the given filter, text colour and opacity and
// one red child(3).
struct Scene {
    Node root{1};
    StubStyles styles;

    explicit Scene(const char* filter, const char* opacity = "1", const char* color = "#0000ff") {
        root.set_bounds({0.0f, 0.0f, 100.0f, 100.0f});
        auto node = std::make_unique<Node>(2);
        node->set_bounds({10.0f, 10.0f, 60.0f, 60.0f});
        auto child = std::make_unique<Node>(3);
        child->set_bounds({0.0f, 0.0f, 30.0f, 30.0f});
        node->add_child(std::move(child));
        root.add_child(std::move(node));
        styles.set(1, styled({{"background-color", "#ffffff"}}));
        restyle(filter, opacity, color);
        styles.set(3, styled({{"background-color", "#ff0000"}}));
        clear_tree(root);
    }
    void restyle(const char* filter, const char* opacity = "1", const char* color = "#0000ff") {
        styles.set(2, styled({{"background-color", "#00ff00"},
                              {"opacity", opacity},
                              {"color", color},
                              {"filter", filter}}));
    }
};

bool dump(const Log& log) {
    for (const auto& entry : log) {
        std::cerr << "  " << entry;
    }
    std::cerr << "\n";
    return false;
}

bool blur_wraps_the_group() {
    Scene scene("blur(4px)");
    LogSink sink(true);
    paint_scene(scene.root, scene.styles, sink);
    const Log want{"fill(100)", "push(100;blur:400)", "fill(100)", "fill(100)", "pop"};
    return sink.log == want ||
           (expect(false, "a blurred node paints as one filtered layer"), dump(sink.log));
}

bool drop_shadow_carries_its_parts() {
    Scene scene("drop-shadow(3px -2px 5px rgba(255, 0, 0, 0.5))");
    LogSink sink(true);
    paint_scene(scene.root, scene.styles, sink);
    return expect(sink.log.size() == 5 && sink.log[1] == "push(100;shadow:300,-200,500,100,0,0,50)",
                  "offset, sigma and colour reach the layer");
}

bool shadow_colour_defaults_to_the_text_colour() {
    Scene omitted("drop-shadow(1px 1px)");
    LogSink a(true);
    paint_scene(omitted.root, omitted.styles, a);
    Scene keyword("drop-shadow(1px 1px 2px currentcolor)");
    LogSink b(true);
    paint_scene(keyword.root, keyword.styles, b);
    return expect(a.log[1] == "push(100;shadow:100,100,0,0,0,100,100)",
                  "an omitted colour is the node's text colour (blue)") &&
           expect(b.log[1] == "push(100;shadow:100,100,200,0,0,100,100)", "so is currentcolor");
}

bool chain_keeps_order_and_folds_blurs() {
    Scene scene("blur(3px) blur(4px) drop-shadow(1px 1px 1px #000) blur(2px)");
    LogSink sink(true);
    paint_scene(scene.root, scene.styles, sink);
    return expect(sink.log[1] == "push(100;blur:500;shadow:100,100,100,0,0,0,100;blur:200)",
                  "adjacent blurs fold (3 and 4 make 5) and the order is kept");
}

bool opacity_moves_to_the_layer() {
    Scene scene("blur(4px)", "0.5");
    LogSink sink(true);
    paint_scene(scene.root, scene.styles, sink);
    return expect(sink.log[1] == "push(50;blur:400)" && sink.log[2] == "fill(100)",
                  "the opacity applies once at the composite, after the filter");
}

bool sigma_is_clamped() {
    Scene scene("blur(500px)");
    PaintContext context;
    context.glass.max_blur_radius = 8.0f;
    LogSink sink(true);
    paint_scene(scene.root, scene.styles, sink, context);
    return expect(sink.log[1] == "push(100;blur:800)", "the sigma obeys glassMaxBlurRadius");
}

bool nothing_to_filter_paints_no_layer() {
    bool ok = true;
    for (const char* value :
         {"none", "blur(0)", "blur(0px)", "brightness(2)", "blur(-1px)", "drop-shadow(", "url(#a)",
          "", "drop-shadow(1px)", "drop-shadow(red)", "drop-shadow(1px 1px -2px red)",
          "blur(1px) drop-shadow(1px 1px 1px red) blur(1px) blur(1px) "
          "drop-shadow(1px 1px 1px red) blur(1px)"}) {
        Scene scene(value);
        LogSink sink(true);
        paint_scene(scene.root, scene.styles, sink);
        const Log want{"fill(100)", "fill(100)", "fill(100)"};
        ok = expect(sink.log == want, value) && ok;
    }
    return ok;
}

bool sink_without_layers_is_unfiltered() {
    Scene scene("blur(4px)");
    LogSink sink(false);
    paint_scene(scene.root, scene.styles, sink);
    const Log want{"fill(100)", "fill(100)", "fill(100)"};
    return expect(sink.log == want, "a sink without layers paints the node unfiltered");
}

bool cache_keeps_filtering() {
    Scene scene("blur(4px)");
    PaintCache cache;
    PaintContext context;
    context.paint_cache = &cache;
    const Log want{"fill(100)", "push(100;blur:400)", "fill(100)", "fill(100)", "pop"};
    bool ok = true;
    for (int frame = 0; frame < 3; ++frame) {
        LogSink sink(true);
        paint_scene(scene.root, scene.styles, sink, context);
        ok = expect(sink.log == want, "every frame, clean or not, carries the filter") && ok;
        clear_tree(scene.root);
    }
    // A restyle to another filter repaints with the new chain, never a stale record.
    scene.restyle("blur(2px)");
    scene.root.children()[0]->mark_dirty(Dirty::Style);
    LogSink changed(true);
    paint_scene(scene.root, scene.styles, changed, context);
    return expect(changed.log[1] == "push(100;blur:200)", "a changed filter is repainted") && ok;
}

bool subtree_list_replays_the_chain() {
    LogSink live(true);
    PaintCache::Recorder recorder(live, true);
    LayerDraw layer{{0.0f, 0.0f, 10.0f, 10.0f}, 1.0f, {}};
    LayerFilterOp shadow;
    shadow.kind = LayerFilterOp::Kind::DropShadow;
    shadow.offset_x = 2.0f;
    shadow.sigma = 1.5f;
    layer.filter.push(shadow);
    recorder.push_layer(layer);
    recorder.fill_rect({0.0f, 0.0f, 10.0f, 10.0f}, Color{1.0f, 0.0f, 0.0f, 1.0f});
    recorder.pop_layer();
    const PaintRecord record = recorder.finish();
    LogSink replayed(true);
    replay_paint_ops(record.ops, record.gradients, replayed);
    return expect(live.log == replayed.log && live.log[0] == "push(100;shadow:200,0,150,0,0,0,100)",
                  "a replayed layer carries the recorded chain");
}

bool sync_widens_the_reach() {
    Scene blur("blur(4px)");
    tessera::ui::paint::sync_scene_style(blur.root, blur.styles, {});
    const Node& node = *blur.root.children()[0];
    const Node::PaintOutsets& around = node.paint_outsets();
    bool ok = expect(around.left == 12.0f && around.top == 12.0f && around.right == 12.0f &&
                         around.bottom == 12.0f,
                     "a blur reaches 3 sigma on every side") &&
              expect(node.glass_blur_sigma() == 4.0f && blur.root.glass_subtree(),
                     "and is a damage dependency of that sigma");

    Scene shadow("drop-shadow(10px 0 2px red)");
    tessera::ui::paint::sync_scene_style(shadow.root, shadow.styles, {});
    const Node::PaintOutsets& side = shadow.root.children()[0]->paint_outsets();
    ok = expect(side.left == 0.0f && side.right == 16.0f && side.top == 6.0f && side.bottom == 6.0f,
                "a shadow reaches 3 sigma around its offset") &&
         ok;

    Scene none("none");
    tessera::ui::paint::sync_scene_style(none.root, none.styles, {});
    return expect(none.root.children()[0]->paint_outsets().left == 0.0f &&
                      none.root.children()[0]->glass_blur_sigma() == 0.0f &&
                      !none.root.glass_subtree(),
                  "no filter, no reach") &&
           ok;
}

} // namespace

int main() {
    const bool ok = blur_wraps_the_group() && drop_shadow_carries_its_parts() &&
                    shadow_colour_defaults_to_the_text_colour() &&
                    chain_keeps_order_and_folds_blurs() && opacity_moves_to_the_layer() &&
                    sigma_is_clamped() && nothing_to_filter_paints_no_layer() &&
                    sink_without_layers_is_unfiltered() && cache_keeps_filtering() &&
                    subtree_list_replays_the_chain() && sync_widens_the_reach();
    if (!ok) {
        return 1;
    }
    std::cout << "paint_filter: ok\n";
    return 0;
}
