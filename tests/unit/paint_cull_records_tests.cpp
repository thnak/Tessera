// SPDX-License-Identifier: MIT
//
// Paint-pass culling against the paint cache (issue #487, #257): a dirty node that owns a cache
// record is painted rather than culled, so its record is refreshed; a dirty node with no record is
// culled like a clean one. Split from paint_cull_tests.cpp to keep that file under the size limit.

#include "paint_cache_test_support.hpp"
#include "tessera/ui/scene/node.hpp"

#include <iostream>
#include <memory>
#include <vector>

namespace paint_cull_records_test {

using paint_cache_test::Call;
using paint_cache_test::clear_all;
using paint_cache_test::CountingStyles;
using paint_cache_test::RecordingSink;
using paint_cache_test::styled;
using tessera::rendering::rhi::Rect;
using tessera::ui::Dirty;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::PaintStats;

namespace {

constexpr NodeId kRoot = 1;
constexpr Rect kView{0.0f, 0.0f, 100.0f, 100.0f};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_cull_records: " << message << "\n";
    }
    return condition;
}

Node& add(Node& parent, NodeId id, Rect bounds) {
    Node& node = parent.add_child(std::make_unique<Node>(id));
    node.set_bounds(bounds);
    return node;
}

bool has_rect(const std::vector<Call>& calls, float x, float y) {
    for (const Call& call : calls) {
        if (call.kind == Call::Kind::Rect && call.rect.x == x && call.rect.y == y) {
            return true;
        }
    }
    return false;
}

bool test_dirty_node_with_a_cache_record_is_not_culled() {
    // Culling a dirty node would skip its re-record, and a later clean paint whose style-free key
    // matches the old entry would replay stale output. The pass therefore paints a dirty node that
    // has a cache record, and only culls it once it is clean.
    Node root(kRoot);
    CountingStyles styles;
    root.set_bounds({0.0f, 0.0f, 400.0f, 400.0f});
    styles.set(kRoot, styled({{"background-color", "#0b1220"}}));
    Node& far_leaf = add(root, 2, {250.0f, 250.0f, 30.0f, 30.0f});
    styles.set(2, styled({{"background-color", "#00ff00"}}));

    paint_cache_test::PaintCache cache;
    PaintContext context;
    context.paint_cache = &cache;
    {
        RecordingSink sink;
        (void)paint_scene(root, styles, sink, context); // records both nodes
    }
    clear_all(root);
    far_leaf.mark_dirty(Dirty::Paint);
    context.cull = kView;
    RecordingSink dirty_sink;
    const PaintStats dirty_stats = paint_scene(root, styles, dirty_sink, context);
    clear_all(root);
    RecordingSink clean_sink;
    const PaintStats clean_stats = paint_scene(root, styles, clean_sink, context);
    return expect(has_rect(dirty_sink.calls, 250.0f, 250.0f),
                  "a dirty node is painted instead of culled, so its record is refreshed") &&
           expect(dirty_stats.culled_nodes == 0, "the dirty node is not counted as culled") &&
           expect(!has_rect(clean_sink.calls, 250.0f, 250.0f),
                  "once clean and outside the rect, the node is culled") &&
           expect(clean_stats.culled_nodes == 1, "the clean node is counted as culled");
}

bool test_cold_dirty_node_is_culled_and_a_dirty_descendant_blocks_the_subtree_skip() {
    // Issue #257: a full repaint culls to the viewport, and the first frame of a scene has every
    // node dirty with an empty cache. A dirty node with no record cannot leave a stale one behind,
    // so it is culled like a clean one; a dirty descendant that does have a record keeps its
    // ancestors' subtree skip off, because skipping would hide it.
    {
        Node root(kRoot);
        CountingStyles styles;
        root.set_bounds({0.0f, 0.0f, 400.0f, 400.0f});
        styles.set(kRoot, styled({{"background-color", "#0b1220"}}));
        Node& far_leaf = add(root, 2, {250.0f, 250.0f, 30.0f, 30.0f});
        styles.set(2, styled({{"background-color", "#00ff00"}}));
        far_leaf.mark_dirty(Dirty::Paint);
        paint_cache_test::PaintCache cache;
        PaintContext context;
        context.paint_cache = &cache;
        context.cull = kView;
        RecordingSink sink;
        const PaintStats stats = paint_scene(root, styles, sink, context);
        if (!expect(!has_rect(sink.calls, 250.0f, 250.0f) && stats.culled_nodes == 1,
                    "a dirty node with no cache record is culled")) {
            return false;
        }
    }
    {
        Node root(kRoot);
        CountingStyles styles;
        root.set_bounds({0.0f, 0.0f, 400.0f, 400.0f});
        styles.set(kRoot, styled({{"background-color", "#0b1220"}}));
        Node& group = add(root, 2, {240.0f, 240.0f, 100.0f, 100.0f});
        styles.set(2, styled({{"background-color", "#223344"}}));
        Node& far_leaf = add(group, 3, {10.0f, 10.0f, 30.0f, 30.0f}); // world 250, 250
        styles.set(3, styled({{"background-color", "#00ff00"}}));
        paint_cache_test::PaintCache cache;
        PaintContext context;
        context.paint_cache = &cache;
        {
            RecordingSink sink;
            (void)paint_scene(root, styles, sink, context); // records every node
        }
        clear_all(root);
        far_leaf.mark_dirty(Dirty::Paint);
        context.cull = kView;
        RecordingSink sink;
        (void)paint_scene(root, styles, sink, context);
        if (!expect(has_rect(sink.calls, 250.0f, 250.0f),
                    "a dirty descendant with a record is painted, not hidden by a subtree skip")) {
            return false;
        }
    }
    return true;
}

} // namespace

} // namespace paint_cull_records_test

int main() {
    bool ok = true;
    ok = paint_cull_records_test::test_dirty_node_with_a_cache_record_is_not_culled() && ok;
    ok = paint_cull_records_test::
             test_cold_dirty_node_is_culled_and_a_dirty_descendant_blocks_the_subtree_skip() &&
         ok;
    if (!ok) {
        std::cerr << "paint_cull_records: failed\n";
        return 1;
    }
    std::cout << "paint_cull_records: ok\n";
    return 0;
}
