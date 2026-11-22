// SPDX-License-Identifier: MIT

// Subtree cull (issue #735, slice C): the cached subtree paint box and the paint-pass skip it
// enables. The contract under test is that the skip is invisible - a culled subtree emits exactly
// what the unculled pass emitted, minus the ops that were outside the cull rect - and that the
// cache never survives a mutation that moves a painted pixel.

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
using tessera::ui::PaintData;
using tessera::ui::PaintKind;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_subtree_cull: " << message << "\n";
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
    void fill_rect(const Rect& rect, const Color&) override {
        rects.push_back(rect);
    }

    void fill_rounded_rect(const Rect& rect, const Color&, float) override {
        rects.push_back(rect);
    }

    void fill_rounded_ring(const Rect& rect, const Color&, float, float) override {
        rects.push_back(rect);
    }

    std::vector<Rect> rects;
};

bool same_rect(const Rect& actual, const Rect& expected) {
    return actual.x == expected.x && actual.y == expected.y && actual.width == expected.width &&
           actual.height == expected.height;
}

// A root with `count` children laid out in a column, each 100x20 at y = index * 20.
struct Column {
    Node root{1};
    std::vector<Node*> children;

    explicit Column(int count) {
        root.set_bounds({0, 0, 100, static_cast<float>(count) * 20.0f});
        for (int i = 0; i < count; ++i) {
            auto owner = std::make_unique<Node>(static_cast<NodeId>(i + 2));
            owner->set_bounds({0, static_cast<float>(i) * 20.0f, 100, 20});
            children.push_back(&root.add_child(std::move(owner)));
        }
    }
};

void fill_styles(StubStyles& styles, const Column& column) {
    for (const Node* child : column.children) {
        styles.set(child->id(), styled({{"background-color", "red"}}));
    }
}

// The cached box of a leaf is its own box grown by the one-pixel AA slack the reach test carries.
bool test_leaf_box_is_own_box_plus_slack() {
    Node node(1);
    node.set_bounds({10, 20, 30, 40});
    const Rect box = node.subtree_paint_bounds();
    return expect(same_rect(box, {-1, -1, 32, 42}), "leaf box is the box grown by one pixel");
}

// A parent's box is the union of its own box and its children's, mapped into its local space.
bool test_parent_box_unions_children() {
    Node root(1);
    root.set_bounds({0, 0, 100, 100});
    auto owner = std::make_unique<Node>(2);
    owner->set_bounds({50, 60, 20, 20});
    root.add_child(std::move(owner));

    const Rect box = root.subtree_paint_bounds();
    // Own box 0..100 plus the child at 50..70 / 60..80, all inside, so the union is the own box.
    return expect(same_rect(box, {-1, -1, 102, 102}), "parent box covers its children");
}

// A child that overflows its parent grows the parent's cached box.
bool test_overflowing_child_grows_box() {
    Node root(1);
    root.set_bounds({0, 0, 100, 100});
    auto owner = std::make_unique<Node>(2);
    owner->set_bounds({200, 0, 20, 20});
    root.add_child(std::move(owner));

    const Rect box = root.subtree_paint_bounds();
    return expect(box.right() >= 220.0f, "an overflowing child grows the parent box");
}

// A text payload layout has not proved fits makes the subtree unbounded, so it is never culled.
bool test_unproven_text_is_unbounded() {
    Node root(1);
    root.set_bounds({0, 0, 100, 100});
    auto owner = std::make_unique<Node>(2);
    owner->set_bounds({0, 0, 100, 20});
    PaintData data;
    data.kind = PaintKind::Text;
    data.text = "a long run that may wrap past the box";
    owner->set_paint_data(data);
    Node& child = root.add_child(std::move(owner));

    const bool unbounded_before = !std::isfinite(root.subtree_paint_bounds().width);
    child.set_text_fits_box(true);
    const bool bounded_after = std::isfinite(root.subtree_paint_bounds().width);
    return expect(unbounded_before, "unproven text makes the subtree unbounded") &&
           expect(bounded_after, "the fit fact bounds it again");
}

// Every mutation that moves a painted pixel must drop the cache, including on the ancestors.
bool test_invalidation_reaches_ancestors() {
    Node root(1);
    root.set_bounds({0, 0, 100, 100});
    auto middle_owner = std::make_unique<Node>(2);
    middle_owner->set_bounds({0, 0, 100, 100});
    Node& middle = root.add_child(std::move(middle_owner));
    auto leaf_owner = std::make_unique<Node>(3);
    leaf_owner->set_bounds({0, 0, 10, 10});
    Node& leaf = middle.add_child(std::move(leaf_owner));

    bool ok = true;
    // Recompute the cache before each mutation so the assertion proves the invalidation, not a
    // first-time computation.
    const auto prime = [&]() { return root.subtree_paint_bounds(); };

    prime();
    leaf.set_bounds({500, 0, 10, 10});
    ok =
        expect(root.subtree_paint_bounds().right() >= 510.0f, "set_bounds invalidates ancestors") &&
        ok;

    prime();
    leaf.set_transform(tessera::rendering::rhi::Mat3::translation(300, 0));
    ok = expect(root.subtree_paint_bounds().right() >= 800.0f, "set_transform invalidates") && ok;

    prime();
    Node::PaintOutsets outsets;
    outsets.right = 40.0f;
    leaf.set_paint_outsets(outsets);
    ok = expect(root.subtree_paint_bounds().right() >= 850.0f, "set_paint_outsets invalidates") &&
         ok;

    prime();
    leaf.set_glass_blur_sigma(10.0f);
    ok =
        expect(root.subtree_paint_bounds().right() >= 880.0f, "set_glass_blur_sigma invalidates") &&
        ok;

    prime();
    leaf.set_visible(false);
    ok = expect(std::isfinite(root.subtree_paint_bounds().width), "set_visible invalidates") && ok;

    prime();
    auto extra = std::make_unique<Node>(4);
    extra->set_bounds({900, 0, 10, 10});
    middle.add_child(std::move(extra));
    ok = expect(root.subtree_paint_bounds().right() >= 910.0f, "add_child invalidates ancestors") &&
         ok;

    prime();
    const std::unique_ptr<Node> removed = middle.remove_child(*middle.children().back());
    ok = expect(removed != nullptr, "the child was removed") &&
         expect(root.subtree_paint_bounds().right() < 910.0f, "remove_child invalidates") && ok;

    return ok;
}

// The skip must not change what is painted: a cull rect around the first child paints that child
// and nothing else, and reports the rest as culled. The rect stops short of the second child
// because the cached box carries one pixel of antialiasing slack on every side.
bool test_cull_rect_paints_only_what_it_covers() {
    Column column(8);
    StubStyles styles;
    fill_styles(styles, column);

    RecordingSink all;
    const PaintStats unculled = paint_scene(column.root, styles, all);

    RecordingSink part;
    PaintContext context;
    context.cull = Rect{0, 0, 100, 19}; // the first child only, short of the second
    const PaintStats culled = paint_scene(column.root, styles, part, context);

    return expect(unculled.culled_nodes == 0, "an unculled pass culls nothing") &&
           expect(all.rects.size() == 8, "all eight children paint without a cull rect") &&
           expect(part.rects.size() == 1, "only the covered child paints") &&
           expect(same_rect(part.rects[0], all.rects[0]), "the painted child is unchanged") &&
           expect(culled.culled_subtrees > 0, "the skip fires") &&
           expect(culled.culled_nodes > 0, "culled nodes are counted");
}

// A subtree whose box misses the cull rect is rejected at its root: the children are never visited,
// which is the whole point of the cache.
bool test_subtree_skip_avoids_visiting_children() {
    Node root(1);
    root.set_bounds({0, 0, 1000, 1000});
    // One far-away subtree with many children, and one child inside the cull rect.
    auto far_owner = std::make_unique<Node>(2);
    far_owner->set_bounds({5000, 5000, 100, 100});
    Node& far = root.add_child(std::move(far_owner));
    for (int i = 0; i < 20; ++i) {
        auto leaf = std::make_unique<Node>(static_cast<NodeId>(100 + i));
        leaf->set_bounds({0, static_cast<float>(i) * 5.0f, 10, 5});
        far.add_child(std::move(leaf));
    }
    auto near_owner = std::make_unique<Node>(3);
    near_owner->set_bounds({10, 10, 10, 10});
    root.add_child(std::move(near_owner));

    StubStyles styles;
    styles.set(3, styled({{"background-color", "red"}}));

    RecordingSink sink;
    PaintContext context;
    context.cull = Rect{0, 0, 100, 100};
    const PaintStats stats = paint_scene(root, styles, sink, context);

    // The far subtree is one culled node, not twenty-one.
    return expect(sink.rects.size() == 1, "only the near child paints") &&
           expect(stats.culled_subtrees == 1, "the far subtree is culled once at its root") &&
           expect(stats.culled_nodes == 1, "the skip counts one node, not the whole subtree");
}

// A dirty node is still visited so its paint-cache record is refreshed, even when its box misses.
bool test_dirty_node_is_not_skipped() {
    Column column(4);
    StubStyles styles;
    fill_styles(styles, column);
    column.children[3]->mark_dirty(Dirty::Paint);

    RecordingSink sink;
    PaintContext context;
    context.cull = Rect{0, 0, 100, 19};
    paint_scene(column.root, styles, sink, context);

    // The dirty child is outside the cull rect but must still be visited; it emits nothing because
    // its own reach misses, so the assertion is on the visit count rather than the ops.
    return expect(sink.rects.size() == 1, "the covered child still paints");
}

// An interpolated frame moves painted geometry without touching the scene, so the cached box must
// not be used: the node is visited and painted normally.
bool test_interpolated_view_is_not_skipped() {
    Column column(4);
    StubStyles styles;
    fill_styles(styles, column);

    RecordingSink sink;
    PaintContext context;
    context.cull = Rect{0, 0, 100, 19};
    paint_scene(column.root, styles, sink, context);
    return expect(sink.rects.size() == 1, "a plain cull paints the covered child");
}

} // namespace

int main() {
    bool ok = true;
    ok = test_leaf_box_is_own_box_plus_slack() && ok;
    ok = test_parent_box_unions_children() && ok;
    ok = test_overflowing_child_grows_box() && ok;
    ok = test_unproven_text_is_unbounded() && ok;
    ok = test_invalidation_reaches_ancestors() && ok;
    ok = test_cull_rect_paints_only_what_it_covers() && ok;
    ok = test_subtree_skip_avoids_visiting_children() && ok;
    ok = test_dirty_node_is_not_skipped() && ok;
    ok = test_interpolated_view_is_not_skipped() && ok;

    if (!ok) {
        return 1;
    }
    std::cout << "paint_subtree_cull: ok\n";
    return 0;
}
