// SPDX-License-Identifier: MIT
//
// Off-screen paint culling (PaintContext::cull, issue #257). The contract under test: whatever a
// culled paint omits cannot be visible inside the cull rect, so the set of draws that survive the
// active clips and the rect is identical to an unculled paint, while far fewer nodes resolve a
// style. The scene is built to hit every way a node outside the rect can still matter: a
// non-clipping parent whose child overflows into view, a clipping parent whose child would
// overflow but is clipped away, a scroll container, and a shadow that reaches in from outside.
// Deterministic and CPU-only: hand-built scene, recording sink, no clock, font or GPU.

#include "paint_cache_test_support.hpp"

#include <algorithm>

namespace paint_cull_test {

using namespace paint_cache_test;

// Ids of the scene below.
constexpr NodeId kOnScreen = 2;
constexpr NodeId kFarLeaf = 3;
constexpr NodeId kOverflowParent = 4;
constexpr NodeId kOverflowChild = 5;
constexpr NodeId kClippedParent = 6;
constexpr NodeId kClippedChild = 7;
constexpr NodeId kScroller = 8;
constexpr NodeId kScrollChild = 9;
constexpr NodeId kShadowed = 10;

Node& add(Node& parent, NodeId id, Rect bounds) {
    Node& node = parent.add_child(std::make_unique<Node>(id));
    node.set_bounds(bounds);
    return node;
}

// The cull rect used throughout is the 100x100 square at the origin.
void build_scene(Node& root, CountingStyles& styles, bool shadow_reach) {
    root.set_bounds({0.0f, 0.0f, 400.0f, 400.0f});

    add(root, kOnScreen, {10.0f, 10.0f, 30.0f, 30.0f});
    styles.set(kOnScreen, styled({{"background-color", "#ff0000"}}));

    add(root, kFarLeaf, {250.0f, 250.0f, 30.0f, 30.0f});
    styles.set(kFarLeaf, styled({{"background-color", "#00ff00"}}));

    // Outside the rect itself, but its child overflows back into view: must still be painted.
    Node& overflow = add(root, kOverflowParent, {300.0f, 300.0f, 20.0f, 20.0f});
    styles.set(kOverflowParent, styled({{"background-color", "#ffff00"}}));
    add(overflow, kOverflowChild, {-280.0f, -280.0f, 20.0f, 20.0f});
    styles.set(kOverflowChild, styled({{"background-color", "#0000ff"}}));

    // Same geometry, but the parent clips: the child's pixels are clipped away, so culling the
    // whole subtree is correct.
    Node& clipped = add(root, kClippedParent, {200.0f, 200.0f, 20.0f, 20.0f});
    styles.set(kClippedParent, styled({{"background-color", "#ff00ff"},
                                       {"overflow-x", "hidden"},
                                       {"overflow-y", "hidden"}}));
    add(clipped, kClippedChild, {-180.0f, -180.0f, 20.0f, 20.0f});
    styles.set(kClippedChild, styled({{"background-color", "#00ffff"}}));

    Node& scroller = add(root, kScroller, {150.0f, 150.0f, 60.0f, 60.0f});
    scroller.set_scrollable_y(true);
    styles.set(kScroller, styled({{"background-color", "#303030"}}));
    add(scroller, kScrollChild, {-140.0f, -140.0f, 20.0f, 20.0f});
    styles.set(kScrollChild, styled({{"background-color", "#808080"}}));

    // Starts at x=110, outside the rect; the shadow reaches 25px left, to x=85, inside it.
    Node& shadowed = add(root, kShadowed, {110.0f, 10.0f, 20.0f, 20.0f});
    styles.set(kShadowed,
               styled({{"background-color", "#101010"}, {"box-shadow", "0 0 0 25px #000000"}}));
    if (shadow_reach) {
        shadowed.set_paint_outsets({25.0f, 25.0f, 25.0f, 25.0f, false});
    }
}

bool intersects(const Rect& a, const Rect& b) {
    return a.x < b.right() && a.right() > b.x && a.y < b.bottom() && a.bottom() > b.y;
}

Rect clip_to(const Rect& a, const Rect& b) {
    const float left = std::max(a.x, b.x);
    const float top = std::max(a.y, b.y);
    const float right = std::min(a.right(), b.right());
    const float bottom = std::min(a.bottom(), b.bottom());
    return {left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
}

struct Visible {
    Call::Kind kind;
    Rect rect;
    Color color;
};

// The draws that can reach a pixel inside `view`: every draw rect cut by the active clip stack and
// by the view, dropped when nothing is left. What culling may omit is exactly what this drops.
std::vector<Visible> visible_draws(const std::vector<Call>& calls, const Rect& view) {
    std::vector<Visible> out;
    std::vector<Rect> clips;
    for (const Call& call : calls) {
        switch (call.kind) {
        case Call::Kind::PushClip:
            clips.push_back(call.clip.rect);
            break;
        case Call::Kind::PopClip:
            clips.pop_back();
            break;
        case Call::Kind::Rect:
        case Call::Kind::Rounded:
        case Call::Kind::Text: {
            Rect shown = clip_to(call.rect, view);
            for (const Rect& clip : clips) {
                shown = clip_to(shown, clip);
            }
            if (shown.width > 0.0f && shown.height > 0.0f) {
                out.push_back({call.kind, call.rect, call.color});
            }
            break;
        }
        default:
            break;
        }
    }
    return out;
}

bool same_visible(const std::vector<Visible>& lhs, const std::vector<Visible>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (lhs[index].kind != rhs[index].kind || !same_rect(lhs[index].rect, rhs[index].rect) ||
            !same_color(lhs[index].color, rhs[index].color)) {
            return false;
        }
    }
    return true;
}

struct Painted {
    std::vector<Call> calls;
    PaintStats stats;
    std::size_t style_calls = 0;
};

Painted paint(bool shadow_reach, std::optional<Rect> cull) {
    Node root(1);
    CountingStyles styles;
    build_scene(root, styles, shadow_reach);
    PaintContext context;
    context.cull = cull;
    RecordingSink sink;
    Painted result;
    result.stats = paint_scene(root, styles, sink, context);
    result.calls = std::move(sink.calls);
    result.style_calls = styles.calls;
    return result;
}

bool test_visible_draws_match_unculled() {
    const Rect view{0.0f, 0.0f, 100.0f, 100.0f};
    const Painted full = paint(true, std::nullopt);
    const Painted culled = paint(true, view);
    const std::vector<Visible> expected = visible_draws(full.calls, view);
    const std::vector<Visible> actual = visible_draws(culled.calls, view);

    return expect(!expected.empty(), "the fixture draws something inside the rect") &&
           expect(same_visible(expected, actual),
                  "the draws visible inside the rect equal the unculled paint") &&
           expect(culled.style_calls < full.style_calls,
                  "culling resolves fewer styles than the full paint") &&
           expect(culled.stats.culled_nodes > 0, "culling reports the nodes it skipped") &&
           expect(culled.calls.size() < full.calls.size(), "culling emits fewer sink calls");
}

bool test_overflowing_child_survives() {
    const Rect view{0.0f, 0.0f, 100.0f, 100.0f};
    const Painted culled = paint(true, view);
    bool child_drawn = false;
    for (const Call& call : culled.calls) {
        child_drawn = child_drawn || (call.kind == Call::Kind::Rect && call.rect.x == 20.0f &&
                                      call.rect.y == 20.0f && call.color.b > 0.9f);
    }
    return expect(child_drawn,
                  "a child overflowing out of a culled, non-clipping parent is still painted");
}

bool test_clipping_subtrees_are_skipped_whole() {
    const Rect view{0.0f, 0.0f, 100.0f, 100.0f};
    const Painted full = paint(true, std::nullopt);
    const Painted culled = paint(true, view);
    return expect(culled.stats.culled_subtrees >= 2,
                  "the overflow-hidden parent and the scroll container skip their subtrees") &&
           expect(culled.stats.nodes < full.stats.nodes, "fewer nodes resolve a style");
}

bool test_shadow_reach_keeps_the_node() {
    const Rect view{0.0f, 0.0f, 100.0f, 100.0f};
    const Painted with_reach = paint(true, view);
    const Painted without_reach = paint(false, view);
    const std::vector<Visible> kept = visible_draws(paint(true, std::nullopt).calls, view);
    return expect(same_visible(kept, visible_draws(with_reach.calls, view)),
                  "a shadow reaching into the rect is painted when its outsets are on the node") &&
           expect(!same_visible(kept, visible_draws(without_reach.calls, view)),
                  "without the synced outsets the shadow would be culled (the dependency is real)");
}

bool test_rect_covering_the_scene_is_byte_identical() {
    const Painted full = paint(true, std::nullopt);
    const Painted culled = paint(true, Rect{-10.0f, -10.0f, 500.0f, 500.0f});
    return expect(same_calls(full.calls, culled.calls),
                  "a cull rect covering every node emits the unculled sequence") &&
           expect(culled.stats.culled_nodes == 0, "nothing is culled");
}

bool test_transformed_node_is_never_culled() {
    Node root(1);
    CountingStyles styles;
    root.set_bounds({0.0f, 0.0f, 400.0f, 400.0f});
    Node& moved = add(root, 2, {300.0f, 300.0f, 20.0f, 20.0f});
    moved.set_transform(Mat3::translation(10.0f, 10.0f));
    styles.set(2, styled({{"background-color", "#ff0000"}}));
    PaintContext context;
    context.cull = Rect{0.0f, 0.0f, 50.0f, 50.0f};
    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink, context);
    // Off-screen even with the matrix, but it opens its own transform frame, and skipping the push
    // would misplace its children, so the prototype paints it.
    return expect(sink.clip_depth == 0, "clips stay balanced") &&
           expect(stats.transforms == 1, "the transformed node still opens its frame") &&
           expect(stats.culled_nodes == 0, "a transformed node is never culled");
}

} // namespace paint_cull_test

int main() {
    bool ok = true;
    ok = paint_cull_test::test_visible_draws_match_unculled() && ok;
    ok = paint_cull_test::test_overflowing_child_survives() && ok;
    ok = paint_cull_test::test_clipping_subtrees_are_skipped_whole() && ok;
    ok = paint_cull_test::test_shadow_reach_keeps_the_node() && ok;
    ok = paint_cull_test::test_rect_covering_the_scene_is_byte_identical() && ok;
    ok = paint_cull_test::test_transformed_node_is_never_culled() && ok;
    if (!ok) {
        std::cerr << "paint_cull: failed\n";
        return 1;
    }
    std::cout << "paint_cull: ok\n";
    return 0;
}
