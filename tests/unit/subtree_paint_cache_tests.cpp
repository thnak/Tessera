// SPDX-License-Identifier: MIT
//
// Retained draw list per clean subtree (issue #476 slice 1, subtree_paint_cache.hpp).
//
// The contract under test: replaying a subtree's list is byte-identical to walking it, a change
// anywhere under it (marked dirty) never replays a stale list, a different cull rect never replays
// a list recorded under another, and a zero budget turns the store off without changing a draw.

#include "paint_cache_test_support.hpp"
#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/subtree_paint_cache.hpp"
#include "tessera/ui/scene/node.hpp"

#include <iostream>

namespace {

using paint_cache_test::build_rich_scene;
using paint_cache_test::clear_all;
using paint_cache_test::CountingStyles;
using paint_cache_test::expect;
using paint_cache_test::RecordingSink;
using paint_cache_test::same_calls;
using tessera::rendering::rhi::Rect;
using tessera::ui::Node;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintCache;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::SubtreePaintCache;
using tessera::ui::paint::SubtreePaintCacheConfig;

struct Fixture {
    Node root{1};
    CountingStyles styles;
    PaintCache cache;
    SubtreePaintCache subtree;
    PaintContext context;

    explicit Fixture(SubtreePaintCacheConfig config = {})
        : subtree(config) {
        build_rich_scene(root, styles);
        context.paint_cache = &cache;
        context.subtree_cache = &subtree;
    }

    RecordingSink paint(PaintStats* stats = nullptr) {
        RecordingSink sink;
        const PaintStats result = paint_scene(root, styles, sink, context);
        if (stats != nullptr) {
            *stats = result;
        }
        return sink;
    }

    RecordingSink paint_uncached() {
        RecordingSink sink;
        PaintContext plain;
        plain.cull = context.cull;
        (void)paint_scene(root, styles, sink, plain);
        return sink;
    }
};

bool test_replay_is_byte_identical() {
    Fixture f;
    const RecordingSink reference = f.paint_uncached();
    PaintStats reference_stats;
    {
        RecordingSink sink;
        PaintContext plain;
        reference_stats = paint_scene(f.root, f.styles, sink, plain);
    }

    (void)f.paint(); // cold: everything dirty, nothing recorded
    clear_all(f.root);
    const RecordingSink recorded = f.paint(); // clean: the list is recorded
    clear_all(f.root);
    f.styles.calls = 0;
    PaintStats stats;
    const RecordingSink replayed = f.paint(&stats); // clean again: the list replays
    const auto counters = f.subtree.stats();

    return expect(same_calls(reference.calls, recorded.calls),
                  "the recording frame is identical") &&
           expect(same_calls(reference.calls, replayed.calls), "the replay is byte-identical") &&
           expect(counters.hits >= 1, "the third frame hit the retained list") &&
           expect(f.styles.calls == 0, "a replay resolves no style") &&
           expect(stats.backgrounds == reference_stats.backgrounds &&
                      stats.borders == reference_stats.borders &&
                      stats.texts == reference_stats.texts && stats.clips == reference_stats.clips,
                  "a replay reports the draw composition of a walk");
}

bool test_change_under_a_list_is_not_replayed_stale() {
    Fixture f;
    (void)f.paint();
    clear_all(f.root);
    (void)f.paint();
    clear_all(f.root);
    (void)f.paint(); // the list now replays

    Node& child = *f.root.children().front();
    const Rect moved{child.bounds().x + 7.0f, child.bounds().y + 5.0f, child.bounds().width,
                     child.bounds().height};
    child.set_bounds(moved); // marks the child and its ancestors
    const RecordingSink reference = f.paint_uncached();
    const RecordingSink after = f.paint();

    return expect(same_calls(reference.calls, after.calls),
                  "a moved descendant repaints; the old list is not replayed");
}

bool test_a_partial_paint_drops_the_lists_it_outdates() {
    Fixture f;
    (void)f.paint();
    clear_all(f.root);
    (void)f.paint();
    clear_all(f.root);
    (void)f.paint(); // the root list exists and replays

    // A restyle painted by a partial repaint (which neither records nor replays), then the tree
    // goes clean: the next full repaint must not bring the pre-restyle list back.
    Node& child = *f.root.children().front();
    f.styles.set(child.id(), paint_cache_test::styled({{"background-color", "#ff00ff"}}));
    child.mark_dirty(tessera::ui::Dirty::Style | tessera::ui::Dirty::Paint);
    f.context.retain_draw_list = false;
    (void)f.paint();
    f.context.retain_draw_list = true;
    clear_all(f.root);
    const RecordingSink reference = f.paint_uncached();
    const RecordingSink after = f.paint();

    return expect(same_calls(reference.calls, after.calls),
                  "a list recorded before a restyle is not replayed after it");
}

bool test_cull_rect_is_part_of_the_key() {
    Fixture f;
    f.context.cull = Rect{0.0f, 0.0f, 240.0f, 180.0f};
    (void)f.paint();
    clear_all(f.root);
    (void)f.paint();
    clear_all(f.root);
    (void)f.paint();

    f.context.cull = Rect{0.0f, 0.0f, 30.0f, 30.0f};
    const RecordingSink reference = f.paint_uncached();
    const RecordingSink culled = f.paint();

    return expect(same_calls(reference.calls, culled.calls),
                  "a list recorded under one cull rect is not replayed under another");
}

bool test_zero_budget_is_off_and_identical() {
    SubtreePaintCacheConfig off;
    off.max_bytes = 0;
    Fixture f(off);
    const RecordingSink reference = f.paint_uncached();
    (void)f.paint();
    clear_all(f.root);
    (void)f.paint();
    clear_all(f.root);
    const RecordingSink third = f.paint();
    const auto counters = f.subtree.stats();

    return expect(same_calls(reference.calls, third.calls), "budget 0 paints identically") &&
           expect(counters.hits == 0 && counters.entries == 0, "budget 0 stores nothing");
}

} // namespace

int main() {
    bool ok = true;
    ok = test_replay_is_byte_identical() && ok;
    ok = test_change_under_a_list_is_not_replayed_stale() && ok;
    ok = test_a_partial_paint_drops_the_lists_it_outdates() && ok;
    ok = test_cull_rect_is_part_of_the_key() && ok;
    ok = test_zero_budget_is_off_and_identical() && ok;
    if (!ok) {
        std::cerr << "subtree_paint_cache: failed\n";
        return 1;
    }
    std::cout << "subtree_paint_cache: ok\n";
    return 0;
}
