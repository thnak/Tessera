// SPDX-License-Identifier: MIT
//
// Retained layers in the paint pass (issue #617, docs/design/gpu/offscreen-layers-retention.md).
// A masked, filtered or isolated-opacity node asks its sink whether it holds the node's pixels
// right before the layer opens; a "yes" skips the box, the children and the mask:
//  - a clean node is kept on its first clean frame and skipped on the next;
//  - a dirty descendant, a moved node, a new epoch and a changed opacity all miss, and a dirty node
//    is reported dirty so the sink can drop what it holds;
//  - retention off (or a sink that never says yes) paints exactly as before;
//  - culling never hides a dirty node from the sink while retention is on;
//  - a subtree draw list records the question, replays it, skips the content on a yes and replays
//    everything on a no, and a recording pass paints the whole layer even when the sink says yes.

#include "tessera/rendering/gpu/mask_composite.hpp"
#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/subtree_paint_cache.hpp"
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
        std::cerr << "paint_layer_retain: " << message << "\n";
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

using Log = std::vector<std::string>;

// A sink that behaves like the renderer's retention: it keeps a layer (owner -> key) when a clean
// layer pops, answers yes for the same key, and forgets an owner reported dirty.
class RetainSink final : public PaintSink {
public:
    void fill_rect(const Rect&, const Color& color) override {
        log.push_back("fill(" + std::to_string(std::lround(color.a * 100.0f)) + ")");
    }
    void fill_rounded_rect(const Rect& rect, const Color& color, float) override {
        fill_rect(rect, color);
    }
    void fill_linear_gradient(const GradientDraw&) override {
        log.push_back("grad");
    }
    [[nodiscard]] bool supports_layers() const override {
        return true;
    }
    [[nodiscard]] bool retain_layer(const LayerRetention& retention) override {
        pending_ = retention;
        recording_ = false;
        if (!retention.clean) {
            held.erase(retention.owner);
            log.push_back("retain(dirty)");
            return false;
        }
        const auto found = held.find(retention.owner);
        if (found != held.end() && found->second == retention.key) {
            log.push_back("retain(hit)");
            return true;
        }
        recording_ = true;
        log.push_back("retain(miss)");
        return false;
    }
    void push_layer(const LayerDraw& layer) override {
        log.push_back("push(" + std::to_string(std::lround(layer.opacity * 100.0f)) + ")");
    }
    void pop_layer() override {
        log.push_back("pop");
        if (recording_) {
            held[pending_.owner] = pending_.key;
            recording_ = false;
        }
    }
    [[nodiscard]] bool begin_layer_mask() override {
        log.push_back("mask{");
        return true;
    }
    void end_layer_mask() override {
        log.push_back("}");
    }
    [[nodiscard]] bool begin_mask_layer(tessera::rendering::gpu::MaskCompositeOp) override {
        log.push_back("part{");
        return parts;
    }
    void end_mask_layer() override {
        log.push_back("}p");
    }
    bool parts = true; // false: a sink that declines non-add mask layers
    Log log;
    std::map<std::uint64_t, std::uint64_t> held;

private:
    LayerRetention pending_;
    bool recording_ = false;
};

constexpr const char* kMask = "linear-gradient(90deg, #000000, #ffffff)";

// root(1) opaque white; node(2) green at (10,10) 60x60 with a mask and one red child(3).
struct Scene {
    Node root{1};
    StubStyles styles;

    explicit Scene(const char* mask = kMask, const char* opacity = "1",
                   const char* composite = "add") {
        root.set_bounds({0.0f, 0.0f, 100.0f, 100.0f});
        auto node = std::make_unique<Node>(2);
        node->set_bounds({10.0f, 10.0f, 60.0f, 60.0f});
        auto child = std::make_unique<Node>(3);
        child->set_bounds({0.0f, 0.0f, 30.0f, 30.0f});
        node->add_child(std::move(child));
        root.add_child(std::move(node));
        styles.set(1, styled({{"background-color", "#ffffff"}}));
        styles.set(2, styled({{"background-color", "#00ff00"},
                              {"opacity", opacity},
                              {"mask-image", mask},
                              {"mask-composite", composite}}));
        styles.set(3, styled({{"background-color", "#ff0000"}}));
        clear_tree(root);
    }
    Node& node() {
        return *root.children()[0];
    }
    Node& child() {
        return *node().children()[0];
    }
};

const Log kFull{"fill(100)", "retain(miss)", "push(100)", "fill(100)", "fill(100)",
                "mask{",     "grad",         "}",         "pop"};
const Log kHit{"fill(100)", "retain(hit)", "push(100)", "pop"};

bool dump(const Log& log) {
    for (const auto& entry : log) {
        std::cerr << "  " << entry;
    }
    std::cerr << "\n";
    return false;
}

PaintContext retaining(PaintCache& cache) {
    PaintContext context;
    context.paint_cache = &cache;
    context.retain_layers = true;
    return context;
}

bool paint(Scene& scene, RetainSink& sink, const PaintContext& context, PaintStats* out = nullptr) {
    const PaintStats stats = paint_scene(scene.root, scene.styles, sink, context);
    if (out != nullptr) {
        *out = stats;
    }
    return true;
}

bool clean_then_hit() {
    Scene scene;
    PaintCache cache;
    const PaintContext context = retaining(cache);
    RetainSink sink;
    PaintStats first;
    paint(scene, sink, context, &first);
    const Log one = sink.log;
    sink.log.clear();
    PaintStats second;
    paint(scene, sink, context, &second);
    return (one == kFull ||
            (expect(false, "the first clean frame paints the layer in full"), dump(one))) &&
           (sink.log == kHit ||
            (expect(false, "the next frame skips box, children and mask"), dump(sink.log))) &&
           expect(second.retained_layers == 1 && first.retained_layers == 0,
                  "the pass counts the retained layer") &&
           expect(second.nodes < first.nodes, "and styles fewer nodes");
}

bool dirty_descendant_misses() {
    Scene scene;
    PaintCache cache;
    const PaintContext context = retaining(cache);
    RetainSink sink;
    paint(scene, sink, context);
    paint(scene, sink, context); // hit
    scene.child().mark_dirty(Dirty::Paint);
    sink.log.clear();
    paint(scene, sink, context);
    const Log dirty{"fill(100)", "retain(dirty)", "push(100)", "fill(100)", "fill(100)",
                    "mask{",     "grad",          "}",         "pop"};
    bool ok = (sink.log == dirty ||
               (expect(false, "a dirty descendant repaints the layer"), dump(sink.log))) &&
              expect(sink.held.empty(), "and the sink was told to drop it");
    clear_tree(scene.root);
    sink.log.clear();
    paint(scene, sink, context);
    ok = (sink.log == kFull ||
          (expect(false, "the first clean frame after it records again"), dump(sink.log))) &&
         ok;
    sink.log.clear();
    paint(scene, sink, context);
    return (sink.log == kHit || (expect(false, "and the next one hits"), dump(sink.log))) && ok;
}

bool inputs_that_move_the_key() {
    // A moved node, a new epoch and a changed opacity each miss; the owner keeps one entry.
    Scene scene;
    PaintCache cache;
    PaintContext context = retaining(cache);
    RetainSink sink;
    paint(scene, sink, context);

    scene.node().set_bounds({12.0f, 10.0f, 60.0f, 60.0f});
    clear_tree(
        scene.root); // the change was seen by damage; the pass sees a clean node at a new key
    sink.log.clear();
    paint(scene, sink, context);
    bool ok = expect(sink.log == kFull, "a moved node misses");
    sink.log.clear();
    paint(scene, sink, context);
    ok = expect(sink.log == kHit, "and hits at its new place") && ok;

    context.retain_epoch = 7;
    sink.log.clear();
    paint(scene, sink, context);
    ok = expect(sink.log == kFull, "a new epoch misses") && ok;

    scene.styles.set(
        2, styled({{"background-color", "#00ff00"}, {"opacity", "0.5"}, {"mask-image", kMask}}));
    scene.node().mark_dirty(Dirty::Style);
    paint(scene, sink, context);
    clear_tree(scene.root);
    sink.log.clear();
    paint(scene, sink, context);
    ok = expect(sink.log.size() == 9 && sink.log[2] == "push(50)" && sink.log[1] == "retain(miss)",
                "a changed opacity re-records") &&
         ok;
    return expect(sink.held.size() == 1, "one entry per owner") && ok;
}

bool off_means_unchanged() {
    Scene scene;
    PaintCache cache;
    PaintContext context = retaining(cache);
    context.retain_layers = false;
    RetainSink sink;
    paint(scene, sink, context);
    paint(scene, sink, context);
    const Log plain{"fill(100)", "push(100)", "fill(100)", "fill(100)",
                    "mask{",     "grad",      "}",         "pop"};
    Log want = plain;
    want.insert(want.end(), plain.begin(), plain.end());
    return (sink.log == want ||
            (expect(false, "no retain call, same stream as before"), dump(sink.log))) &&
           expect(sink.held.empty(), "nothing is kept");
}

bool culling_never_hides_a_dirty_node() {
    Scene scene;
    PaintCache cache;
    PaintContext context = retaining(cache);
    RetainSink sink;
    paint(scene, sink, context);                     // records (no cull)
    context.cull = Rect{80.0f, 80.0f, 10.0f, 10.0f}; // far from the node at (10,10)-(70,70)
    sink.log.clear();
    paint(scene, sink, context);
    bool ok = expect(sink.log.size() <= 1, "a clean node outside the cull rect is skipped");
    scene.child().mark_dirty(Dirty::Paint);
    sink.log.clear();
    paint(scene, sink, context);
    bool saw_dirty = false;
    for (const auto& entry : sink.log) {
        saw_dirty = saw_dirty || entry == "retain(dirty)";
    }
    return expect(saw_dirty, "a dirty one is still seen, so the sink can drop its pixels") &&
           expect(sink.held.empty(), "and it did") && ok;
}

bool opacity_group_is_retainable() {
    Scene scene("none", "0.5");
    PaintCache cache;
    PaintContext context = retaining(cache);
    context.isolate_group_opacity = true;
    RetainSink sink;
    paint(scene, sink, context);
    const Log recorded = sink.log;
    sink.log.clear();
    paint(scene, sink, context);
    const Log want_hit{"fill(100)", "retain(hit)", "push(50)", "pop"};
    return expect(recorded.size() == 6 && recorded[1] == "retain(miss)" &&
                      recorded[2] == "push(50)",
                  "an isolated opacity group asks too") &&
           (sink.log == want_hit || (expect(false, "and is skipped when kept"), dump(sink.log)));
}

bool subtree_list_asks_again() {
    Scene scene;
    PaintCache cache;
    SubtreePaintCache lists;
    PaintContext context = retaining(cache);
    context.subtree_cache = &lists;
    context.retain_draw_list = true;

    RetainSink sink;
    paint(scene, sink, context); // records the root's list while the sink records the layer
    const Log recorded = sink.log;
    sink.log.clear();
    paint(scene, sink, context); // replays the list; the sink says yes
    bool ok =
        expect(recorded == kFull, "recording forwards the whole layer to a sink that says no") &&
        (sink.log == kHit ||
         (expect(false, "a replay asks again and skips on yes"), dump(sink.log))) &&
        expect(lists.stats().hits == 1, "from the retained list");

    // The sink forgot (evicted): the same list replays the whole layer.
    RetainSink forgot;
    paint(scene, forgot, context);
    ok = (forgot.log == kFull ||
          (expect(false, "a replay on a sink that says no draws it all"), dump(forgot.log))) &&
         ok;

    // A recording pass over a sink that already says yes still records the whole layer, and
    // forwards none of it.
    Scene fresh;
    SubtreePaintCache other_lists;
    PaintCache other_cache;
    PaintContext other = retaining(other_cache);
    other.subtree_cache = &other_lists;
    other.retain_draw_list = true;
    RetainSink knows;
    paint(fresh, knows, other); // learns the layer (miss -> kept)
    fresh.root.mark_dirty(Dirty::Paint);
    clear_tree(fresh.root);
    other_lists.clear();
    knows.log.clear();
    paint(fresh, knows, other); // list is re-recorded while the sink says yes
    ok = (knows.log == kHit ||
          (expect(false, "recording over a yes forwards only the composite"), dump(knows.log))) &&
         ok;
    RetainSink empty;
    paint(fresh, empty, other); // replay of what was recorded under the yes
    return (empty.log == kFull ||
            (expect(false, "that list still holds the whole layer for a sink that lets go"),
             dump(empty.log))) &&
           ok;
}

// A mask with a non-add layer: the part bracket rides inside the mask bracket inside the layer.
constexpr const char* kTwoLayers = "linear-gradient(#000, #fff), linear-gradient(#fff, #000)";
const Log kSubtractFull{"fill(100)", "retain(miss)", "push(100)", "fill(100)", "fill(100)", "mask{",
                        "grad",      "part{",        "grad",      "}p",        "}",         "pop"};

bool brackets_balanced(const Log& log) {
    int parts = 0;
    int masks = 0;
    for (const auto& entry : log) {
        parts += entry == "part{" ? 1 : entry == "}p" ? -1 : 0;
        masks += entry == "mask{" ? 1 : entry == "}" ? -1 : 0;
    }
    return parts == 0 && masks == 0;
}

bool retained_layer_skips_its_mask_parts() {
    Scene scene(kTwoLayers, "1", "subtract");
    PaintCache cache;
    const PaintContext context = retaining(cache);
    RetainSink sink;
    paint(scene, sink, context);
    const Log first = sink.log;
    sink.log.clear();
    paint(scene, sink, context);
    return (first == kSubtractFull ||
            (expect(false, "a clean subtract mask paints its part inside the mask"),
             dump(first))) &&
           (sink.log == kHit ||
            (expect(false, "a kept layer skips the mask and its parts"), dump(sink.log)));
}

bool subtree_list_keeps_part_brackets_balanced() {
    Scene scene(kTwoLayers, "1", "subtract");
    PaintCache cache;
    SubtreePaintCache lists;
    PaintContext context = retaining(cache);
    context.subtree_cache = &lists;
    context.retain_draw_list = true;

    RetainSink sink;
    paint(scene, sink, context); // records the list; the sink records the layer
    bool ok = expect(sink.log == kSubtractFull, "recording forwards the whole layer");
    sink.log.clear();
    paint(scene, sink, context); // replay: the sink says yes
    ok = (sink.log == kHit || (expect(false, "a replayed yes skips every part"), dump(sink.log))) &&
         ok;

    RetainSink forgot; // the sink let go: the same list replays the layer, parts included
    paint(scene, forgot, context);
    ok = (forgot.log == kSubtractFull ||
          (expect(false, "a replay on a sink that lost it paints the parts"), dump(forgot.log))) &&
         ok;

    RetainSink no_parts; // a sink that declines the part: its content and end are skipped
    no_parts.parts = false;
    paint(scene, no_parts, context);
    const Log declined{"fill(100)", "retain(miss)", "push(100)", "fill(100)", "fill(100)",
                       "mask{",     "grad",         "part{",     "}",         "pop"};
    ok = (no_parts.log == declined ||
          (expect(false, "a declined part is skipped with its end"), dump(no_parts.log))) &&
         ok;

    // A recording pass over a sink that already says yes forwards only the composite, and the list
    // it recorded still carries balanced brackets for a sink that lets go.
    Scene fresh(kTwoLayers, "1", "intersect");
    SubtreePaintCache other_lists;
    PaintCache other_cache;
    PaintContext other = retaining(other_cache);
    other.subtree_cache = &other_lists;
    other.retain_draw_list = true;
    RetainSink knows;
    paint(fresh, knows, other);
    fresh.root.mark_dirty(Dirty::Paint);
    clear_tree(fresh.root);
    other_lists.clear();
    knows.log.clear();
    paint(fresh, knows, other);
    ok = (knows.log == kHit ||
          (expect(false, "recording over a yes forwards no part"), dump(knows.log))) &&
         ok;
    RetainSink empty;
    paint(fresh, empty, other);
    return expect(empty.log == kSubtractFull ||
                      (empty.log.size() == kSubtractFull.size() && brackets_balanced(empty.log)),
                  "the recorded list replays balanced part brackets") &&
           ok;
}

} // namespace

int main() {
    const bool ok = clean_then_hit() && dirty_descendant_misses() && inputs_that_move_the_key() &&
                    off_means_unchanged() && culling_never_hides_a_dirty_node() &&
                    opacity_group_is_retainable() && subtree_list_asks_again() &&
                    retained_layer_skips_its_mask_parts() &&
                    subtree_list_keeps_part_brackets_balanced();
    if (!ok) {
        return 1;
    }
    std::cout << "paint_layer_retain: ok\n";
    return 0;
}
