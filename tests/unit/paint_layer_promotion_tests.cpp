// SPDX-License-Identifier: MIT
//
// Generic layer promotion (issue #967, docs/design/gpu/offscreen-layers-retention.md section 11).
// The policy is deterministic (draws, stable frames, area, no glass) and the paint pass sends a
// qualifying clean container down the retained-layer path:
//  - nothing is promoted until the container was clean for `min_stable_frames` painted frames;
//  - a promoted container records once, then hits;
//  - a container that turns dirty paints flat and, once it is promoted again, misses (a layer
//    recorded before the change never comes back);
//  - few draws, a small area, a glass backdrop inside, an LCD context, a fade, a nested layer and
//    a missing policy are all left flat;
//  - the table is bounded and counts its resets.

#include "tessera/ui/paint/layer_promotion.hpp"
#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/subtree_paint_cache.hpp"
#include "tessera/ui/scene/node.hpp"

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>

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
        std::cerr << "paint_layer_promotion: " << message << "\n";
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

// Behaves like the renderer's retention: keeps (owner -> key) when a clean layer pops.
class RetainSink final : public PaintSink {
public:
    void fill_rect(const Rect&, const Color&) override {}
    void fill_rounded_rect(const Rect&, const Color&, float) override {}
    [[nodiscard]] bool supports_layers() const override {
        return true;
    }
    [[nodiscard]] bool retain_layer(const LayerRetention& retention) override {
        pending_ = retention;
        recording_ = false;
        if (!retention.clean) {
            held.erase(retention.owner);
            ++dirty;
            return false;
        }
        const auto found = held.find(retention.owner);
        if (found != held.end() && found->second == retention.key) {
            ++hits;
            return true;
        }
        recording_ = true;
        ++misses;
        return false;
    }
    void release_layer(std::uint64_t owner) override {
        released += held.erase(owner) != 0 ? 1 : 0;
        ++release_calls;
    }
    void push_layer(const LayerDraw&) override {
        ++layers;
    }
    void pop_layer() override {
        if (recording_) {
            held[pending_.owner] = pending_.key;
            recording_ = false;
        }
    }
    void reset_counts() {
        hits = misses = dirty = layers = released = release_calls = 0;
    }
    int hits = 0;
    int misses = 0;
    int dirty = 0;
    int layers = 0;
    int released = 0;      // retained entries dropped by release_layer
    int release_calls = 0; // release_layer calls, held or not
    std::map<std::uint64_t, std::uint64_t> held;

private:
    LayerRetention pending_;
    bool recording_ = false;
};

// root(1) 100x100; card(2) 200x200 with `children` 10x10 children that each paint a background.
struct Scene {
    Node root{1};
    StubStyles styles;

    explicit Scene(int children = 40, const char* card_opacity = "1",
                   const char* child_extra = nullptr) {
        // Smaller than `min_area`, so only the card is a candidate.
        root.set_bounds({0.0f, 0.0f, 100.0f, 100.0f});
        auto card = std::make_unique<Node>(2);
        card->set_bounds({10.0f, 10.0f, 200.0f, 200.0f});
        for (int index = 0; index < children; ++index) {
            auto child = std::make_unique<Node>(static_cast<NodeId>(10 + index));
            child->set_bounds({static_cast<float>(index % 10) * 12.0f,
                               static_cast<float>(index / 10) * 12.0f, 10.0f, 10.0f});
            ComputedStyle style = styled({{"background-color", "#ff0000"}});
            if (child_extra != nullptr && index == 0) {
                style.set("backdrop-filter", child_extra);
            }
            styles.set(child->id(), std::move(style));
            card->add_child(std::move(child));
        }
        root.add_child(std::move(card));
        styles.set(1, styled({{"background-color", "#ffffff"}}));
        styles.set(2, styled({{"background-color", "#00ff00"}, {"opacity", card_opacity}}));
        clear_tree(root);
    }
    Node& card() {
        return *root.children()[0];
    }
};

struct Run {
    PaintCache cache;
    LayerPromotion policy;
    PaintContext context;
    RetainSink sink;

    explicit Run(LayerPromotionConfig config = {})
        : policy(config) {
        context.paint_cache = &cache;
        context.retain_layers = true;
        context.layer_promotion = &policy;
    }
    PaintStats frame(Scene& scene) {
        sink.reset_counts();
        return paint_scene(scene.root, scene.styles, sink, context);
    }
};

bool earns_its_promotion() {
    Scene scene;
    Run run;
    bool ok = true;
    for (int index = 1; index <= 3; ++index) {
        const PaintStats stats = run.frame(scene);
        ok = expect(run.sink.layers == 0 && run.sink.hits + run.sink.misses == 0 &&
                        stats.promoted_layers == 0,
                    "a container is flat until it has been clean for min_stable_frames") &&
             ok;
    }
    PaintStats stats = run.frame(scene);
    ok = expect(run.sink.misses == 1 && run.sink.layers == 1 && stats.promoted_layers == 1,
                "the next frame promotes it and the sink records") &&
         ok;
    stats = run.frame(scene);
    ok = expect(run.sink.hits == 1 && stats.retained_layers == 1 && stats.promoted_layers == 1,
                "the frame after composites the retained pixels") &&
         ok;
    ok = expect(stats.nodes < 5, "and a hit styles almost none of the 41 nodes") && ok;
    const LayerPromotionStats policy = run.policy.stats();
    return expect(policy.promotable == 1 && policy.promotions == 1 && policy.demotions == 0,
                  "the policy counts one promotion") &&
           ok;
}

bool dirty_node_never_hits_a_stale_layer() {
    Scene scene;
    Run run;
    for (int index = 0; index < 6; ++index) {
        run.frame(scene); // flat x3, record, hit, hit
    }
    scene.card().children()[3]->mark_dirty(Dirty::Paint);
    PaintStats stats = run.frame(scene);
    bool ok = expect(run.sink.layers == 0 && run.sink.hits + run.sink.misses == 0 &&
                         stats.promoted_layers == 0,
                     "a dirty subtree paints flat, no layer");
    // Issue #1014: the entry recorded for the promoted node goes the frame it turns dirty.
    ok = expect(run.sink.released == 1 && run.sink.held.count(2) == 0,
                "the stale retained entry is released at once, not left to eviction") &&
         ok;
    run.frame(scene); // still dirty: nothing left to release
    ok = expect(run.sink.release_calls == 0, "a node already demoted does not notify again") && ok;
    clear_tree(scene.root);
    for (int index = 0; index < 3; ++index) {
        run.frame(scene);
    }
    ok = expect(run.sink.layers == 0, "and is flat again until it re-earns the promotion") && ok;
    run.frame(scene);
    ok = expect(run.sink.misses == 1 && run.sink.hits == 0,
                "the layer recorded before the change does not come back: a miss") &&
         ok;
    run.frame(scene);
    ok = expect(run.sink.hits == 1, "then it hits") && ok;
    return expect(run.policy.stats().demotions == 1, "the demotion was counted") && ok;
}

bool stays_flat_when_it_should() {
    bool ok = true;
    {
        Scene scene(10); // 11 draws
        Run run;
        for (int index = 0; index < 8; ++index) {
            run.frame(scene);
        }
        ok = expect(run.sink.layers == 0 && run.policy.stats().rejected_small > 0,
                    "too few draws") &&
             ok;
    }
    {
        Scene scene;
        LayerPromotionConfig config;
        config.min_area = 200.0f * 200.0f + 1.0f;
        Run run(config);
        for (int index = 0; index < 8; ++index) {
            run.frame(scene);
        }
        ok = expect(run.sink.layers == 0, "too small an area") && ok;
    }
    {
        Scene scene(40, "1", "blur(4px)");
        Run run;
        for (int index = 0; index < 8; ++index) {
            run.frame(scene);
        }
        ok = expect(run.sink.layers == 0 && run.policy.stats().rejected_backdrop > 0,
                    "a glass backdrop inside is never promoted") &&
             ok;
    }
    {
        Scene scene(40, "0.5");
        Run run;
        for (int index = 0; index < 8; ++index) {
            run.frame(scene);
        }
        ok = expect(run.sink.layers == 0, "a faded container keeps its flat fade") && ok;
    }
    {
        Scene scene;
        Run run;
        run.context.lcd_text = true;
        for (int index = 0; index < 8; ++index) {
            run.frame(scene);
        }
        ok = expect(run.sink.layers == 0, "LCD text cannot ride in a layer") && ok;
    }
    {
        Scene scene;
        Run run;
        run.context.layer_promotion = nullptr;
        for (int index = 0; index < 8; ++index) {
            run.frame(scene);
        }
        ok = expect(run.sink.layers == 0 && run.sink.hits + run.sink.misses == 0,
                    "no policy, no promotion: exactly the flat paint") &&
             ok;
    }
    {
        Scene scene;
        Run run;
        run.context.retain_layers = false;
        for (int index = 0; index < 8; ++index) {
            run.frame(scene);
        }
        ok = expect(run.sink.layers == 0, "retention off turns promotion off") && ok;
    }
    return ok;
}

bool a_new_epoch_restarts_the_count() {
    Scene scene;
    Run run;
    for (int index = 0; index < 4; ++index) {
        run.frame(scene);
    }
    run.context.retain_epoch = 9;
    run.frame(scene);
    bool ok = expect(run.sink.layers == 0, "a new epoch is flat again");
    for (int index = 0; index < 3; ++index) {
        run.frame(scene);
    }
    return expect(run.sink.misses == 1, "and promotes after min_stable_frames more") && ok;
}

bool works_over_the_subtree_draw_list() {
    // The pipeline records clean subtrees as flat lists; a replayed list still reports its draws,
    // so the container is observed and, once promoted, leaves the list for the layer. A list
    // records the outermost clean subtree, so here that is the root, and the root is what is
    // promoted.
    Scene scene;
    scene.root.set_bounds({0.0f, 0.0f, 400.0f, 400.0f});
    clear_tree(scene.root);
    Run run;
    SubtreePaintCache lists;
    run.context.subtree_cache = &lists;
    for (int index = 0; index < 3; ++index) {
        run.frame(scene);
    }
    bool ok = expect(run.sink.layers == 0 && run.policy.stats().entries == 1,
                     "flat frames are observed whether painted or replayed from a list");
    run.frame(scene);
    ok = expect(run.sink.misses == 1, "it is promoted over the list") && ok;
    const PaintStats stats = run.frame(scene);
    return expect(run.sink.hits == 1 && stats.retained_layers == 1, "and then hits") && ok;
}

bool the_table_is_bounded() {
    LayerPromotionConfig config;
    config.max_entries = 2;
    LayerPromotion policy(config);
    LayerPromotionObservation seen;
    seen.draws = 100;
    seen.area = 1.0e6f;
    seen.clean = true;
    for (NodeId id = 1; id <= 5; ++id) {
        (void)policy.observe(id, seen);
    }
    const LayerPromotionStats stats = policy.stats();
    const std::uint64_t before = policy.key_epoch(5, 0);
    policy.clear();
    return expect(stats.entries <= 2 && stats.table_resets == 2,
                  "a full table is cleared, counted") &&
           expect(policy.key_epoch(5, 0) != before, "and a clear moves every key epoch");
}

} // namespace

int main() {
    const bool ok = earns_its_promotion() && dirty_node_never_hits_a_stale_layer() &&
                    stays_flat_when_it_should() && a_new_epoch_restarts_the_count() &&
                    works_over_the_subtree_draw_list() && the_table_is_bounded();
    if (!ok) {
        return 1;
    }
    std::cout << "paint_layer_promotion: ok\n";
    return 0;
}
