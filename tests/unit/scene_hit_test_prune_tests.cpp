// SPDX-License-Identifier: MIT

// `Node::hit_test_deep` maps the point one level at a time and prunes subtrees by the cached
// subtree paint box. The contract under test is that this is invisible: it returns exactly the
// node the straightforward algorithm (invert the whole parent chain at every node, visit every
// subtree) returns. The reference below is that straightforward algorithm, written against the
// public API only, and both run over deterministic random trees that mix the cases pruning could
// get wrong: children outside the parent's box, transforms, shadows, z-index, scroll containers,
// hidden nodes, `pointer-events: none`, and text whose paint is not bounded.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/scene/z_order.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using tessera::rendering::rhi::Mat3;
using tessera::rendering::rhi::Rect;
using tessera::ui::for_each_by_z;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "scene_hit_test_prune: " << message << "\n";
    }
    return condition;
}

// The pre-pruning algorithm: every node inverts its own world transform and every subtree is
// visited.
Node* reference_hit(Node& node, float x, float y) {
    if (!node.rendered()) {
        return nullptr;
    }
    if (node.scrollable_x() || node.scrollable_y()) {
        Mat3 inverse;
        if (!node.world_transform().try_inverse(inverse)) {
            return nullptr;
        }
        float local_x = 0;
        float local_y = 0;
        inverse.transform_point(x, y, local_x, local_y);
        const float min_x = std::min(node.scrollport_left(), node.bounds().width);
        const float min_y = std::min(node.scrollport_top(), node.bounds().height);
        const float max_x = std::max(min_x, node.bounds().width - node.scrollport_right());
        const float max_y = std::max(min_y, node.bounds().height - node.scrollport_bottom());
        if (local_x < min_x || local_y < min_y || local_x > max_x || local_y > max_y) {
            return node.pointer_events() && node.hit_test(x, y) ? &node : nullptr;
        }
    }
    Node* hit = nullptr;
    for_each_by_z(node.children(), true, [&](Node& child) {
        hit = reference_hit(child, x, y);
        return hit != nullptr;
    });
    if (hit != nullptr) {
        return hit;
    }
    return node.pointer_events() && node.hit_test(x, y) ? &node : nullptr;
}

class Random {
public:
    explicit Random(std::uint64_t seed)
        : state_(seed) {}
    std::uint32_t next() {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(state_ >> 33);
    }
    // [0, n)
    std::uint32_t below(std::uint32_t n) {
        return next() % n;
    }
    // [lo, hi)
    float real(float lo, float hi) {
        return lo + (hi - lo) * (static_cast<float>(next() % 100000) / 100000.0f);
    }

private:
    std::uint64_t state_;
};

struct Tree {
    std::unique_ptr<Node> root;
    std::size_t nodes = 0;
};

void decorate(Node& node, Random& random, bool allow_extremes) {
    // About a third of the nodes carry a transform, which takes the general inversion path.
    switch (random.below(6)) {
    case 0:
        node.set_transform(Mat3::translation(random.real(-20, 20), random.real(-20, 20)));
        break;
    case 1:
        node.set_transform(Mat3::rotation(random.real(-0.8f, 0.8f)));
        break;
    case 2: {
        const float scale = random.real(0.5f, 1.8f);
        node.set_transform(Mat3::scale(scale, scale));
        break;
    }
    default:
        break;
    }
    if (random.below(5) == 0) {
        node.set_z_index(static_cast<int>(random.below(5)) - 2);
    }
    if (random.below(6) == 0) {
        const float reach = random.real(0, 30);
        node.set_paint_outsets({reach, reach, reach, reach, random.below(2) == 0});
    }
    if (random.below(8) == 0) {
        node.set_glass_blur_sigma(random.real(0, 8));
    }
    if (allow_extremes) {
        if (random.below(12) == 0) {
            node.set_pointer_events(false);
        }
        if (random.below(15) == 0) {
            node.set_visible(false);
        }
        if (random.below(15) == 0) {
            node.set_style_hidden(true);
        }
        // Text that layout never proved fits its box: its subtree paint box is unbounded, so the
        // prune must keep the whole subtree.
        if (random.below(10) == 0) {
            PaintData data;
            data.kind = PaintKind::Text;
            data.text = "overflowing text";
            node.set_paint_data(data);
        }
    }
}

void grow(Node& parent, Random& random, int depth, NodeId& next_id, std::size_t& count) {
    if (depth == 0) {
        return;
    }
    const std::uint32_t children = 1 + random.below(depth > 3 ? 4 : 3);
    for (std::uint32_t i = 0; i < children; ++i) {
        auto child = std::make_unique<Node>(next_id++);
        ++count;
        // Children are placed freely, many of them outside the parent's box.
        child->set_bounds(Rect{random.real(-60, 220), random.real(-60, 160), random.real(5, 140),
                               random.real(5, 110)});
        decorate(*child, random, true);
        Node& attached = parent.add_child(std::move(child));
        if (random.below(9) == 0) {
            attached.set_scrollable_y(true);
            attached.set_scrollable_x(random.below(2) == 0);
            attached.set_content_size(600, 600);
            attached.set_scroll_offset(random.real(0, 120), random.real(0, 120));
            if (random.below(2) == 0) {
                attached.set_scrollport_insets(2, 2, 2, 2);
            }
        }
        grow(attached, random, depth - 1, next_id, count);
    }
}

Tree make_tree(std::uint64_t seed) {
    Random random(seed);
    Tree tree;
    tree.root = std::make_unique<Node>(1);
    tree.root->set_bounds(Rect{0, 0, 320, 240});
    NodeId next_id = 2;
    tree.nodes = 1;
    grow(*tree.root, random, 5, next_id, tree.nodes);
    return tree;
}

bool test_random_trees_match_reference() {
    std::size_t queries = 0;
    std::size_t hits = 0;
    for (std::uint64_t seed = 1; seed <= 60; ++seed) {
        Tree tree = make_tree(seed * 7919);
        Random points(seed * 104729);
        for (int i = 0; i < 400; ++i) {
            const float x = points.real(-80, 400);
            const float y = points.real(-80, 320);
            Node* expected = reference_hit(*tree.root, x, y);
            Node* actual = tree.root->hit_test_deep(x, y);
            ++queries;
            hits += expected != nullptr ? 1 : 0;
            if (expected != actual) {
                std::cerr << "scene_hit_test_prune: seed " << seed << " point (" << x << ", " << y
                          << ") expected id " << (expected != nullptr ? expected->id() : 0)
                          << " got id " << (actual != nullptr ? actual->id() : 0) << "\n";
                return false;
            }
        }
    }
    // A comparison that never hits proves nothing; the trees are dense enough that many queries do.
    return expect(hits > queries / 4, "the random trees must produce hits, not only misses");
}

bool test_entry_below_root_matches_reference() {
    // The entry node is not the root: its point is mapped through the whole parent chain, which
    // includes an ancestor's scroll offset and transform.
    Tree tree = make_tree(424242);
    Random points(99);
    for (const auto& child : tree.root->children()) {
        for (int i = 0; i < 200; ++i) {
            const float x = points.real(-80, 400);
            const float y = points.real(-80, 320);
            if (reference_hit(*child, x, y) != child->hit_test_deep(x, y)) {
                return expect(false, "a non-root entry node must match the reference");
            }
        }
    }
    return true;
}

bool test_child_outside_parent_box_is_still_hit() {
    // The prune uses the subtree paint box, not the parent's own box: a child placed outside its
    // parent is painted and hit there, and the parent must not hide it.
    Node root(1);
    root.set_bounds(Rect{0, 0, 100, 100});
    auto parent = std::make_unique<Node>(2);
    parent->set_bounds(Rect{0, 0, 20, 20});
    auto child = std::make_unique<Node>(3);
    child->set_bounds(Rect{200, 200, 30, 30}); // far outside the 20 x 20 parent
    Node& child_ref = parent->add_child(std::move(child));
    root.add_child(std::move(parent));
    return expect(root.hit_test_deep(215, 215) == &child_ref,
                  "a child outside its parent's box must still be hit");
}

bool test_shadow_reach_does_not_create_hits() {
    // Outsets enlarge the paint box, not the hit area: a point inside the shadow but outside the
    // box hits nothing.
    Node root(1);
    root.set_bounds(Rect{0, 0, 200, 200});
    auto card = std::make_unique<Node>(2);
    card->set_bounds(Rect{50, 50, 40, 40});
    card->set_paint_outsets({20, 20, 20, 20, true});
    Node& card_ref = root.add_child(std::move(card));
    return expect(root.hit_test_deep(60, 60) == &card_ref, "inside the box is a hit") &&
           expect(root.hit_test_deep(40, 60) == &root, "inside the shadow only falls through");
}

bool test_unbounded_text_subtree_is_not_pruned() {
    // Unproven text makes the subtree box infinite, so a descendant far outside the nominal box is
    // still reachable.
    Node root(1);
    root.set_bounds(Rect{0, 0, 500, 500});
    auto host = std::make_unique<Node>(2);
    host->set_bounds(Rect{0, 0, 10, 10});
    PaintData data;
    data.kind = PaintKind::Text;
    data.text = "wraps well past its box";
    host->set_paint_data(data);
    auto far_child = std::make_unique<Node>(3);
    far_child->set_bounds(Rect{300, 300, 20, 20});
    Node& far_ref = host->add_child(std::move(far_child));
    root.add_child(std::move(host));
    return expect(root.hit_test_deep(310, 310) == &far_ref,
                  "an unbounded subtree must be searched in full");
}

bool test_transformed_subtree_is_hit_where_it_paints() {
    Node root(1);
    root.set_bounds(Rect{0, 0, 300, 300});
    auto holder = std::make_unique<Node>(2);
    holder->set_bounds(Rect{100, 100, 40, 40});
    holder->set_transform(Mat3::scale(3.0f, 3.0f)); // paints 120 x 120 from (100, 100)
    auto inner = std::make_unique<Node>(3);
    inner->set_bounds(Rect{30, 30, 10, 10});
    Node& inner_ref = holder->add_child(std::move(inner));
    root.add_child(std::move(holder));
    // inner spans (30..40) in holder space, so (190..220) in the root after scaling by 3.
    return expect(root.hit_test_deep(205, 205) == &inner_ref,
                  "a scaled subtree is hit at its painted position") &&
           expect(root.hit_test_deep(150, 150)->id() == 2, "the scaled holder itself is hit");
}

bool test_stale_cache_is_not_trusted_after_a_move() {
    // The subtree box is cached; moving a descendant must invalidate it or the prune would hide
    // the node at its new position.
    Node root(1);
    root.set_bounds(Rect{0, 0, 400, 400});
    auto holder = std::make_unique<Node>(2);
    holder->set_bounds(Rect{0, 0, 20, 20});
    auto mover = std::make_unique<Node>(3);
    mover->set_bounds(Rect{5, 5, 10, 10});
    Node& mover_ref = holder->add_child(std::move(mover));
    root.add_child(std::move(holder));
    if (!expect(root.hit_test_deep(10, 10) == &mover_ref, "hit before the move")) {
        return false; // also fills the subtree box cache
    }
    mover_ref.set_bounds(Rect{300, 300, 10, 10});
    return expect(root.hit_test_deep(305, 305) == &mover_ref, "hit at the new position") &&
           expect(root.hit_test_deep(10, 10)->id() == 2, "no hit left at the old position");
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_random_trees_match_reference();
    ok &= test_entry_below_root_matches_reference();
    ok &= test_child_outside_parent_box_is_still_hit();
    ok &= test_shadow_reach_does_not_create_hits();
    ok &= test_unbounded_text_subtree_is_not_pruned();
    ok &= test_transformed_subtree_is_hit_where_it_paints();
    ok &= test_stale_cache_is_not_trusted_after_a_move();
    return ok ? 0 : 1;
}
