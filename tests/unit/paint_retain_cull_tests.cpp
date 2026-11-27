// SPDX-License-Identifier: MIT
//
// Retention must not defeat viewport culling (issue #617): with retain_layers on (the pipeline
// default) a large dirty subtree that holds no retained layer, under an off-screen parent, is not
// painted. The first frame of a scene (everything dirty) and the next one alike. A dirty
// off-screen retained owner is still visited so it can drop its layer: see paint_layer_retain.

#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <iostream>
#include <map>
#include <memory>
#include <utility>

namespace {

using tessera::rendering::rhi::Rect;
using tessera::ui::Node;
using tessera::ui::NodeId;
using namespace tessera::ui::paint;
using tessera::ui::style::ComputedStyle;

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

class NullSink final : public PaintSink {
public:
    void fill_rect(const Rect&, const tessera::rendering::rhi::Color&) override {}
    void fill_rounded_rect(const Rect&, const tessera::rendering::rhi::Color&, float) override {}
};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_retain_cull: " << message << "\n";
    }
    return condition;
}

bool dirty_offscreen_subtree_stays_culled() {
    Node root{1};
    root.set_bounds({0.0f, 0.0f, 100.0f, 100.0f});
    auto far_parent = std::make_unique<Node>(2);
    far_parent->set_bounds({500.0f, 500.0f, 100.0f, 100.0f});
    for (NodeId id = 10; id < 410; ++id) {
        auto leaf = std::make_unique<Node>(id);
        leaf->set_bounds({1.0f, 1.0f, 5.0f, 5.0f});
        far_parent->add_child(std::move(leaf));
    }
    root.add_child(std::move(far_parent));
    StubStyles styles;
    ComputedStyle white;
    white.set("background-color", "#ffffff");
    styles.set(1, white);
    styles.set(2, white);
    PaintCache cache;
    PaintContext context;
    context.paint_cache = &cache;
    context.retain_layers = true;
    context.cull = Rect{0.0f, 0.0f, 50.0f, 50.0f};
    NullSink sink;
    const PaintStats first = paint_scene(root, styles, sink, context); // everything is dirty
    const PaintStats second = paint_scene(root, styles, sink, context);
    return expect(first.nodes < 5 && first.culled_nodes >= 1,
                  "a dirty off-screen subtree without retained layers stays culled") &&
           expect(second.nodes < 5 && second.culled_nodes >= 1, "and on the next frame");
}

} // namespace

int main() {
    if (!dirty_offscreen_subtree_stays_culled()) {
        return 1;
    }
    std::cout << "paint_retain_cull: ok\n";
    return 0;
}
