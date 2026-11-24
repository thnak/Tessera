// SPDX-License-Identifier: MIT
//
// The scoped interaction sync and the scoped transition bridge (issue #735): they walk only the
// restyled roots, and fall back to the whole tree as soon as a node's `visible()` or the scene's
// shape changed since the last whole walk, because those are the two things besides a style change
// that can owe a node outside the roots anything.
//
// Deterministic: a manual clock, a map-backed style provider, no GPU.

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/animation/runtime.hpp"
#include "tessera/ui/animation/style_bridge.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/structure_epoch.hpp"

#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::rhi::Extent2D;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::DirtyPass;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::StyleBridge;
using tessera::ui::frame::FramePipeline;
using tessera::ui::frame::FrameTick;
using tessera::ui::layout::DamageTracker;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "scoped_style_sync: " << message << "\n";
    }
    return condition;
}

// A provider over a per-node table; `resolve` returns a stable reference like the host's memo.
class Styles final : public tessera::ui::paint::StyleProvider {
public:
    [[nodiscard]] ComputedStyle compute(const Node& node) override {
        return table_[node.id()];
    }
    [[nodiscard]] const ComputedStyle& resolve(const Node& node) override {
        return table_[node.id()];
    }
    void set(NodeId id, const char* property, const char* value) {
        ComputedStyle next = table_[id];
        next.set(property, value);
        table_[id] = next; // a fresh entries block, so its identity differs from the last one
    }

private:
    std::unordered_map<NodeId, ComputedStyle> table_;
};

const ComputedStyle& lookup(void* context, const Node& node) {
    return static_cast<Styles*>(context)->resolve(node);
}

// root(1) -> a(2), b(3) -> c(4)
struct Scene {
    Node root{1};
    Node* a = nullptr;
    Node* b = nullptr;
    Node* c = nullptr;
    Scene() {
        a = &root.add_child(std::make_unique<Node>(2));
        b = &root.add_child(std::make_unique<Node>(3));
        c = &b->add_child(std::make_unique<Node>(4));
        root.set_bounds({0.0f, 0.0f, 400.0f, 400.0f});
    }
};

struct Pipeline {
    ManualClock clock;
    RuntimeLoop loop{clock};
    DamageTracker tracker;
    DamageAccumulator damage{DamageConfig{Extent2D{400, 400}, 8}};
    DirtyPass dirty;
    AnimationRuntime animations;
    FramePipeline pipeline{animations, loop, tracker, damage, dirty};
};

bool test_a_scoped_walk_touches_only_the_roots() {
    Scene scene;
    Styles styles;
    Pipeline p;
    p.pipeline.sync_style(scene.root, styles, FrameTick{}, true); // the whole walk
    styles.set(2, "pointer-events", "none");
    styles.set(3, "pointer-events", "none");
    const Node* roots[] = {scene.a};
    p.pipeline.sync_style_scoped(scene.root, styles, roots, FrameTick{}, true);
    return expect(!scene.a->pointer_events(), "a root takes its new interaction state") &&
           expect(scene.b->pointer_events(), "a node outside the roots is left alone");
}

bool test_a_visibility_flip_falls_back_to_the_whole_tree() {
    Scene scene;
    Styles styles;
    Pipeline p;
    p.pipeline.sync_style(scene.root, styles, FrameTick{}, true);
    scene.b->set_visible(false);
    scene.b->set_visible(true);
    styles.set(4, "pointer-events", "none");
    const Node* roots[] = {scene.a};
    p.pipeline.sync_style_scoped(scene.root, styles, roots, FrameTick{}, true);
    return expect(!scene.c->pointer_events(),
                  "after a visible() flip the next sync walks the whole tree");
}

bool test_a_structure_change_falls_back_to_the_whole_tree() {
    Scene scene;
    Styles styles;
    Pipeline p;
    p.pipeline.sync_style(scene.root, styles, FrameTick{}, true);
    scene.a->add_child(std::make_unique<Node>(5));
    styles.set(4, "pointer-events", "none");
    const Node* roots[] = {scene.a};
    p.pipeline.sync_style_scoped(scene.root, styles, roots, FrameTick{}, true);
    return expect(!scene.c->pointer_events(), "after a shape change the next sync walks the tree");
}

bool test_a_root_below_a_hidden_node_is_skipped() {
    Scene scene;
    Styles styles;
    Pipeline p;
    p.pipeline.sync_style(scene.root, styles, FrameTick{}, true);
    styles.set(3, "visibility", "hidden");
    styles.set(4, "pointer-events", "none");
    const Node* whole[] = {scene.b};
    p.pipeline.sync_style_scoped(scene.root, styles, whole, FrameTick{}, true);
    const Node* below[] = {scene.c};
    p.pipeline.sync_style_scoped(scene.root, styles, below, FrameTick{}, true);
    return expect(scene.b->style_hidden(), "the hidden node itself is applied") &&
           expect(scene.c->pointer_events(), "a root below a hidden node is not walked");
}

bool test_the_bridge_scoped_walk_keeps_every_snapshot() {
    Scene scene;
    Styles styles;
    for (NodeId id : {NodeId{1}, NodeId{2}, NodeId{3}, NodeId{4}}) {
        styles.set(id, "transition", "opacity 200ms linear");
    }
    ManualClock clock;
    RuntimeLoop loop(clock);
    AnimationRuntime animations;
    StyleBridge bridge;
    const std::vector<tessera::ui::style::KeyframeBlock> none;
    (void)bridge.sync(animations, scene.root, &lookup, &styles, none);
    if (!expect(bridge.tracked() == 4, "every node declaring a transition is tracked")) {
        return false;
    }
    const Node* roots[] = {scene.a};
    (void)bridge.sync_scoped(animations, roots, &lookup, &styles, none);
    return expect(bridge.tracked() == 4, "a scoped walk does not erase what it did not reach");
}

bool test_visibility_epoch_moves_only_on_a_real_flip() {
    Scene scene;
    const std::uint64_t before = tessera::ui::scene_visibility_epoch();
    scene.a->set_visible(true); // already visible
    if (!expect(tessera::ui::scene_visibility_epoch() == before, "a no-op does not bump")) {
        return false;
    }
    scene.a->set_visible(false);
    return expect(tessera::ui::scene_visibility_epoch() == before + 1, "a flip bumps once");
}

} // namespace

int main() {
    bool ok = true;
    ok = test_a_scoped_walk_touches_only_the_roots() && ok;
    ok = test_a_visibility_flip_falls_back_to_the_whole_tree() && ok;
    ok = test_a_structure_change_falls_back_to_the_whole_tree() && ok;
    ok = test_a_root_below_a_hidden_node_is_skipped() && ok;
    ok = test_the_bridge_scoped_walk_keeps_every_snapshot() && ok;
    ok = test_visibility_epoch_moves_only_on_a_real_flip() && ok;
    if (!ok) {
        std::cerr << "scoped_style_sync: failed\n";
        return 1;
    }
    std::cout << "scoped_style_sync: ok\n";
    return 0;
}
