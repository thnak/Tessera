// SPDX-License-Identifier: MIT
//
// Layer `filter` as its own dependency (issue #617, docs/design/gpu/offscreen-layers.md section
// 5d). A filtered node used to borrow the glass machinery (its reach / 3 was stored as the backdrop
// sigma), so a tree with a `filter` looked like a tree with glass and the window rendered it
// through the offscreen frame target. These tests pin the split:
//  - the style sync stores the filter's reach in its own node field and its own subtree flag, and
//    never touches the backdrop sigma or `glass_subtree` (so the window has nothing to react to);
//  - a node can carry both and neither value leaks into the other;
//  - the halo adds to the node's own shadow reach and also surrounds an overflowing child;
//  - damage touching the halo repaints the whole filtered region, damage outside it does not, and
//  the
//    glass walk ignores filters (and the filter walk ignores glass);
//  - the paint cull keeps a node whose halo is on screen and drops one whose halo is not;
//  - the frame pipeline accounts the filter dependency separately from the glass one.

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/layout/damage_pass.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/scene_style_sync.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Rect;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::Dirty;
using tessera::ui::DirtyPass;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::frame::FramePipeline;
using tessera::ui::frame::FramePipelineConfig;
using tessera::ui::frame::FrameRender;
using tessera::ui::layout::DamageTracker;
using tessera::ui::layout::node_filter_region;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::sync_scene_style;
using tessera::ui::paint::sync_scene_style_scoped;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "filter_damage: " << message << "\n";
    }
    return condition;
}

bool same_rect(const Rect& lhs, const Rect& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width && lhs.height == rhs.height;
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

// root(1) 400x200 > card(2) at (50,50) 40x40 > far child(3) overflowing to x = 200; plus a sibling
// (4) far from the card. The card carries `filter` and optionally a backdrop-filter.
struct Scene {
    Node root{1};
    StubStyles styles;

    explicit Scene(const char* filter, const char* backdrop = "none", const char* shadow = "none") {
        root.set_bounds({0.0f, 0.0f, 400.0f, 200.0f});
        auto card = std::make_unique<Node>(2);
        card->set_bounds({50.0f, 50.0f, 40.0f, 40.0f});
        auto child = std::make_unique<Node>(3);
        child->set_bounds({150.0f, 0.0f, 10.0f, 10.0f});
        card->add_child(std::move(child));
        root.add_child(std::move(card));
        auto far = std::make_unique<Node>(4);
        far->set_bounds({300.0f, 150.0f, 10.0f, 10.0f});
        root.add_child(std::move(far));
        styles.set(1, styled({{"background-color", "#101010"}}));
        restyle(filter, backdrop, shadow);
        styles.set(3, styled({{"background-color", "#ff0000"}}));
        styles.set(4, styled({{"background-color", "#00ff00"}}));
        clear_tree(root);
    }
    void restyle(const char* filter, const char* backdrop = "none", const char* shadow = "none") {
        styles.set(2, styled({{"background-color", "#00ff00"},
                              {"filter", filter},
                              {"backdrop-filter", backdrop},
                              {"box-shadow", shadow}}));
    }
    Node& card() {
        return *root.children()[0];
    }
};

bool filter_is_its_own_dependency() {
    Scene scene("blur(4px)");
    const auto result = sync_scene_style(scene.root, scene.styles, {});
    const Node& card = scene.card();
    bool ok = expect(card.filter_reach() == 12.0f, "a blur(4px) reaches 3 sigma = 12 px") &&
              expect(card.glass_blur_sigma() == 0.0f, "the backdrop sigma stays 0") &&
              expect(!card.glass_subtree() && !scene.root.glass_subtree(),
                     "a filter does not make a glass subtree") &&
              expect(card.filter_subtree() && scene.root.filter_subtree(),
                     "it makes a filter subtree up to the root") &&
              expect(!scene.root.children()[1]->filter_subtree(), "a sibling is not one") &&
              expect(result.glass_nodes == 0 && result.filter_nodes == 1,
                     "glass_nodes counts glass only; filter_nodes counts filters") &&
              expect(result.changed_filter == 1 && result.changed_glass == 0, "one reach changed");

    const auto again = sync_scene_style(scene.root, scene.styles, {});
    ok = expect(again.changed_filter == 0 && again.changed_outsets == 0,
                "a steady scene changes nothing (no rounding churn)") &&
         ok;

    // A reach that is not a whole step must not look changed every frame.
    scene.restyle("blur(1.3px)");
    sync_scene_style(scene.root, scene.styles, {});
    const auto steady = sync_scene_style(scene.root, scene.styles, {});
    ok = expect(steady.changed_filter == 0 && scene.card().filter_reach() >= 3.9f &&
                    scene.card().filter_reach() <= 4.2f,
                "a fractional reach rounds up once and then stays") &&
         ok;

    scene.restyle("none");
    sync_scene_style(scene.root, scene.styles, {});
    return expect(scene.card().filter_reach() == 0.0f && !scene.root.filter_subtree(),
                  "removing the filter clears the reach and the subtree flag") &&
           ok;
}

bool glass_and_filter_do_not_leak() {
    Scene scene("blur(4px)", "blur(2px)");
    const auto result = sync_scene_style(scene.root, scene.styles, {});
    const Node& card = scene.card();
    return expect(card.glass_blur_sigma() == 2.0f && card.filter_reach() == 12.0f,
                  "each keeps its own value") &&
           expect(card.glass_subtree() && card.filter_subtree(), "and its own subtree flag") &&
           expect(result.glass_nodes == 1 && result.filter_nodes == 1, "and its own counter");
}

bool glass_alone_is_unchanged() {
    Scene scene("none", "blur(4px)");
    const auto result = sync_scene_style(scene.root, scene.styles, {});
    return expect(scene.card().glass_blur_sigma() == 4.0f && scene.card().filter_reach() == 0.0f,
                  "a backdrop blur is not a filter") &&
           expect(scene.root.glass_subtree() && !scene.root.filter_subtree(),
                  "glass keeps its subtree flag only") &&
           expect(result.glass_nodes == 1 && result.filter_nodes == 0, "glass is counted as glass");
}

bool scoped_sync_refolds_both_flags() {
    Scene scene("none");
    sync_scene_style(scene.root, scene.styles, {});
    scene.restyle("blur(2px)", "blur(2px)");
    const std::vector<const Node*> roots{&scene.card()};
    sync_scene_style_scoped(scene.styles, roots, {});
    bool ok = expect(scene.root.filter_subtree() && scene.root.glass_subtree(),
                     "ancestors of a restyled root refold both flags");
    scene.restyle("none", "none");
    sync_scene_style_scoped(scene.styles, roots, {});
    return expect(!scene.root.filter_subtree() && !scene.root.glass_subtree(),
                  "and clear them again") &&
           ok;
}

bool halo_adds_to_the_shadow() {
    Scene scene("blur(2px)", "none", "0 0 0 6px #000");
    sync_scene_style(scene.root, scene.styles, {});
    const Node::PaintOutsets& outsets = scene.card().paint_outsets();
    // The layer holds the node's own shadow and the filter blurs it, so the reaches add (6 + 6)
    // instead of the larger one winning.
    return expect(outsets.left == 12.0f && outsets.bottom == 12.0f,
                  "shadow reach and filter halo add per side") &&
           expect(scene.card().filter_reach() == 6.0f, "the filter field holds the halo alone");
}

bool region_follows_the_subtree() {
    Scene scene("blur(4px)");
    sync_scene_style(scene.root, scene.styles, {});
    const Rect region = node_filter_region(scene.card(), 1.0f);
    // The card's box (50..90) with its halo, and the child that overflows to x = 50 + 160 = 210,
    // also with the halo: 12 px past 210, plus the one pixel of anti-aliasing slack.
    bool ok = expect(region.x <= 38.0f && region.y <= 38.0f, "the halo surrounds the box") &&
              expect(region.right() >= 222.0f, "the halo also surrounds an overflowing child");
    const Rect none = node_filter_region(*scene.root.children()[1], 1.0f);
    return expect(none.width == 0.0f && none.height == 0.0f, "no filter, no region") && ok;
}

bool damage_in_the_halo_repaints_the_region() {
    Scene scene("blur(4px)");
    sync_scene_style(scene.root, scene.styles, {});
    DamageAccumulator damage(DamageConfig{Extent2D{400, 200}, 16});
    DamageTracker tracker;

    damage.add({300.0f, 150.0f, 10.0f, 10.0f}); // the far sibling
    bool ok = expect(tracker.add_filter_dependencies(scene.root, damage, 1.0f, 0.0f) == 0,
                     "damage away from the halo leaves the filter alone");

    damage.add({40.0f, 60.0f, 4.0f, 4.0f}); // in the halo, left of the box
    ok = expect(tracker.add_filter_dependencies(scene.root, damage, 1.0f, 0.0f) == 1,
                "damage in the halo repaints the filtered node") &&
         ok;
    const Rect region = node_filter_region(scene.card(), 1.0f);
    bool whole = false;
    for (const Rect& rect : damage.rects()) {
        whole = whole || (rect.x <= region.x && rect.y <= region.y &&
                          rect.right() >= region.right() && rect.bottom() >= region.bottom());
    }
    ok = expect(whole, "the whole region is added, not just the touched part") && ok;

    // The overflowing child's own halo counts as the node's halo.
    Scene overflow("blur(4px)");
    sync_scene_style(overflow.root, overflow.styles, {});
    DamageAccumulator child_damage(DamageConfig{Extent2D{400, 200}, 16});
    child_damage.add(
        {212.0f, 52.0f, 4.0f, 4.0f}); // past the child (right edge 210), inside its halo
    DamageTracker child_tracker;
    ok = expect(child_tracker.add_filter_dependencies(overflow.root, child_damage, 1.0f, 0.0f) == 1,
                "damage in an overflowing child's halo repaints the filtered node") &&
         ok;
    return ok;
}

bool the_two_walks_are_independent() {
    Scene filtered("blur(4px)");
    sync_scene_style(filtered.root, filtered.styles, {});
    DamageAccumulator damage(DamageConfig{Extent2D{400, 200}, 16});
    damage.add({40.0f, 60.0f, 4.0f, 4.0f});
    DamageTracker tracker;
    bool ok = expect(tracker.add_glass_dependencies(filtered.root, damage, 1.0f, 0.0f) == 0,
                     "the glass walk ignores a filter-only tree");

    Scene glass("none", "blur(4px)");
    sync_scene_style(glass.root, glass.styles, {});
    DamageAccumulator glass_damage(DamageConfig{Extent2D{400, 200}, 16});
    glass_damage.add({40.0f, 60.0f, 4.0f, 4.0f});
    ok = expect(tracker.add_filter_dependencies(glass.root, glass_damage, 1.0f, 0.0f) == 0,
                "the filter walk ignores a glass-only tree") &&
         ok;
    return expect(tracker.add_glass_dependencies(glass.root, glass_damage, 1.0f, 0.0f) == 1,
                  "and glass is still handled by its own walk") &&
           ok;
}

class RectSink final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color&) override {
        rects.push_back(rect);
    }
    void fill_rounded_rect(const Rect& rect, const Color&, float) override {
        rects.push_back(rect);
    }
    std::vector<Rect> rects;
};

std::size_t paint_with_cull(Scene& scene, const Rect& cull) {
    RectSink sink;
    PaintContext context;
    context.cull = cull;
    paint_scene(scene.root, scene.styles, sink, context);
    std::size_t card_rects = 0;
    for (const Rect& rect : sink.rects) {
        card_rects += same_rect(rect, Rect{50.0f, 50.0f, 40.0f, 40.0f}) ? 1 : 0;
    }
    return card_rects;
}

bool cull_keeps_a_node_whose_halo_is_on_screen() {
    Scene scene("blur(4px)");
    sync_scene_style(scene.root, scene.styles, {});
    // The box starts at x = 50; its halo reaches x = 38.
    const std::size_t halo_visible = paint_with_cull(scene, {0.0f, 0.0f, 44.0f, 200.0f});
    const std::size_t halo_off = paint_with_cull(scene, {0.0f, 0.0f, 30.0f, 200.0f});
    Scene plain("none");
    sync_scene_style(plain.root, plain.styles, {});
    const std::size_t plain_at_44 = paint_with_cull(plain, {0.0f, 0.0f, 44.0f, 200.0f});
    return expect(halo_visible == 1, "a node whose halo reaches the cull rect is painted") &&
           expect(halo_off == 0, "one whose halo is off screen is culled") &&
           expect(plain_at_44 == 0, "the same box without a filter is culled there");
}

struct Pipeline {
    ManualClock clock;
    RuntimeLoop loop{clock};
    AnimationRuntime animations;
    DamageAccumulator damage{DamageConfig{Extent2D{400, 200}, 16}};
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipeline pipeline{animations, loop, tracker, damage, dirty, FramePipelineConfig{}};
};

bool frame_pipeline_accounts_filters_separately() {
    Scene scene("blur(4px)");
    Pipeline state;
    state.pipeline.sync_style(scene.root, scene.styles, state.pipeline.tick(scene.root, 1.0 / 60.0),
                              true);
    const FrameRender first = state.pipeline.finish_frame(scene.root);
    bool ok = expect(first.should_render, "the first frame renders") &&
              expect(first.filter_dependencies == 1 && first.glass_dependencies == 0,
                     "a filter frame accounts a filter dependency and no glass one");

    clear_tree(scene.root);
    state.pipeline.tick(scene.root, 1.0 / 60.0);
    const FrameRender idle = state.pipeline.finish_frame(scene.root);
    ok = expect(!idle.should_render && idle.filter_dependencies == 0,
                "a static filtered screen presents zero frames") &&
         ok;

    // A change inside the halo repaints the filtered node; one far away does not.
    scene.root.children()[1]->mark_dirty(Dirty::Paint | Dirty::Render);
    state.pipeline.tick(scene.root, 1.0 / 60.0);
    const FrameRender far = state.pipeline.finish_frame(scene.root);
    ok = expect(far.should_render && far.filter_dependencies == 0,
                "a far change does not touch the filter") &&
         ok;
    clear_tree(scene.root);
    scene.card().children()[0]->mark_dirty(Dirty::Paint | Dirty::Render);
    state.pipeline.tick(scene.root, 1.0 / 60.0);
    const FrameRender near = state.pipeline.finish_frame(scene.root);
    return expect(near.should_render && near.filter_dependencies == 1,
                  "a change under the filter repaints it") &&
           ok;
}

} // namespace

int main() {
    const bool ok = filter_is_its_own_dependency() && glass_and_filter_do_not_leak() &&
                    glass_alone_is_unchanged() && scoped_sync_refolds_both_flags() &&
                    halo_adds_to_the_shadow() && region_follows_the_subtree() &&
                    damage_in_the_halo_repaints_the_region() && the_two_walks_are_independent() &&
                    cull_keeps_a_node_whose_halo_is_on_screen() &&
                    frame_pipeline_accounts_filters_separately();
    return ok ? 0 : 1;
}
