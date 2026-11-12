// SPDX-License-Identifier: MIT
//
// Per-node paint cache (ui/paint/paint_cache.hpp): byte-identical replay against an uncached paint,
// a clean subtree that records nothing and resolves no style, and the invalidation matrix (dirty
// flags, bounds, transform, inherited clip, metrics source, PaintView). Budgets/LRU/determinism are
// in paint_cache_bounds_tests.cpp (same executable). No GPU, wall clock, system font, network or
// random ids: the scene is built by hand and the sink records every call verbatim.

#include "paint_cache_test_support.hpp"

namespace paint_cache_test {

bool test_byte_identical_replay() {
    Node root(1);
    CountingStyles styles;
    build_rich_scene(root, styles);

    RecordingSink uncached;
    const PaintStats uncached_stats = paint_scene(root, styles, uncached);
    const std::size_t visible = count_nodes(root);

    PaintCache cache;
    PaintContext context;
    context.paint_cache = &cache;

    styles.calls = 0;
    RecordingSink cold;
    const PaintStats cold_stats = paint_scene(root, styles, cold, context);
    const std::size_t cold_style_calls = styles.calls;
    const PaintCacheStats after_cold = cache.stats();

    clear_all(root);
    styles.calls = 0;
    RecordingSink warm;
    const PaintStats warm_stats = paint_scene(root, styles, warm, context);
    const PaintCacheStats after_warm = cache.stats();

    const bool counters_replayed =
        warm_stats.backgrounds == uncached_stats.backgrounds &&
        warm_stats.borders == uncached_stats.borders && warm_stats.clips == uncached_stats.clips &&
        warm_stats.texts == uncached_stats.texts && warm_stats.vectors == uncached_stats.vectors &&
        warm_stats.images == uncached_stats.images &&
        warm_stats.gradients == uncached_stats.gradients &&
        warm_stats.shadows == uncached_stats.shadows &&
        warm_stats.transforms == uncached_stats.transforms;

    return expect(!uncached.calls.empty(), "the fixture emits a non-trivial sequence") &&
           expect(same_calls(uncached.calls, cold.calls),
                  "a recording paint emits the uncached sequence") &&
           expect(same_calls(uncached.calls, warm.calls),
                  "a warm paint replays the byte-identical sequence") &&
           expect(cold_stats.backgrounds == uncached_stats.backgrounds, "cold stats match") &&
           expect(cold_style_calls == visible, "cold paint resolves one style per visible node") &&
           expect(styles.calls == 0, "warm paint resolves no style") &&
           expect(warm_stats.nodes == 0, "warm paint counts no resolved node") &&
           expect(counters_replayed, "warm paint replays the draw counters") &&
           expect(after_warm.hits - after_cold.hits == visible, "every visible node hits") &&
           expect(after_warm.misses == after_cold.misses, "warm paint adds no miss") &&
           expect(after_warm.recorded_draws == after_cold.recorded_draws,
                  "warm paint records no new sink draw") &&
           expect(after_warm.replayed_draws - after_cold.replayed_draws == uncached.calls.size(),
                  "warm paint replays exactly the cold sink calls") &&
           expect(warm.clip_depth == 0, "replayed clips stay balanced");
}

// Second paint of an entirely clean subtree: zero new records, zero style resolutions, output
// equal.
bool test_clean_subtree_is_not_re_recorded() {
    Node root(1);
    CountingStyles styles;
    build_rich_scene(root, styles);

    PaintCache cache;
    PaintContext context;
    context.paint_cache = &cache;

    RecordingSink first;
    paint_scene(root, styles, first, context);
    clear_all(root);

    const PaintCacheStats before = cache.stats();
    styles.calls = 0;
    RecordingSink second;
    const PaintStats stats = paint_scene(root, styles, second, context);
    const PaintCacheStats after = cache.stats();

    return expect(same_calls(first.calls, second.calls), "clean subtree output is identical") &&
           expect(after.recorded_draws == before.recorded_draws, "zero new sink draws recorded") &&
           expect(after.misses == before.misses, "zero lookups missed") &&
           expect(styles.calls == 0, "zero style resolutions") &&
           expect(stats.nodes == 0, "PaintStats::nodes drops to zero") &&
           expect(!first.calls.empty() && !second.calls.empty(), "the replay is not vacuous");
}

bool test_dirty_node_re_records_changed_output() {
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 100.0f, 100.0f});
    auto first_owner = std::make_unique<Node>(2);
    first_owner->set_bounds({0.0f, 0.0f, 20.0f, 20.0f});
    Node& first = root.add_child(std::move(first_owner));
    auto second_owner = std::make_unique<Node>(3);
    second_owner->set_bounds({30.0f, 0.0f, 20.0f, 20.0f});
    root.add_child(std::move(second_owner));

    CountingStyles styles;
    styles.set(1, styled({{"background-color", "#111111"}}));
    styles.set(2, styled({{"background-color", "#ff0000"}}));
    styles.set(3, styled({{"background-color", "#00ff00"}}));

    PaintCache cache;
    PaintContext context;
    context.paint_cache = &cache;
    RecordingSink cold;
    paint_scene(root, styles, cold, context);
    clear_all(root);
    RecordingSink warm;
    paint_scene(root, styles, warm, context);

    // Paint-only change on node 2: the caller updates the style and marks the domain dirty.
    styles.set(2, styled({{"background-color", "#0000ff"}}));
    first.mark_dirty(Dirty::Paint);
    const PaintCacheStats before = cache.stats();
    styles.calls = 0;
    RecordingSink changed;
    const PaintStats stats = paint_scene(root, styles, changed, context);
    const PaintCacheStats after = cache.stats();

    bool repainted = false;
    for (const Call& call : changed.calls) {
        if (call.kind == Call::Kind::Rect && same_rect(call.rect, {0.0f, 0.0f, 20.0f, 20.0f}) &&
            same_color(call.color, rgba8(0, 0, 255))) {
            repainted = true;
        }
    }
    return expect(styles.calls == 1, "only the dirty node re-resolves its style") &&
           expect(after.misses - before.misses == 1, "only the dirty node misses") &&
           expect(after.hits - before.hits == 2, "the clean siblings still hit") &&
           expect(stats.nodes == 1, "one node was re-recorded") &&
           expect(repainted, "the changed output appears in the new frame") &&
           expect(same_calls(warm.calls, cold.calls), "the pre-change frames agreed");
}

bool test_bounds_change_misses() {
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 100.0f, 100.0f});
    auto child_owner = std::make_unique<Node>(2);
    child_owner->set_bounds({10.0f, 10.0f, 20.0f, 20.0f});
    Node& child = root.add_child(std::move(child_owner));

    CountingStyles styles;
    styles.set(1, styled({{"background-color", "#111111"}}));
    styles.set(2, styled({{"background-color", "#ff0000"}}));

    PaintCache cache;
    PaintContext context;
    context.paint_cache = &cache;
    RecordingSink cold;
    paint_scene(root, styles, cold, context);
    clear_all(root);

    child.set_bounds({40.0f, 50.0f, 15.0f, 25.0f});
    const PaintCacheStats before = cache.stats();
    styles.calls = 0;
    RecordingSink moved;
    paint_scene(root, styles, moved, context);
    const PaintCacheStats after = cache.stats();

    bool at_new_rect = false;
    for (const Call& call : moved.calls) {
        if (call.kind == Call::Kind::Rect && same_rect(call.rect, {40.0f, 50.0f, 15.0f, 25.0f})) {
            at_new_rect = true;
        }
    }
    return expect(at_new_rect, "the moved node is emitted at its new bounds") &&
           expect(styles.calls == 1, "the moved node re-resolves its style") &&
           expect(after.misses - before.misses == 1, "the moved node misses") &&
           expect(after.hits - before.hits == 1, "the unchanged root still hits");
}

bool test_transform_change_misses() {
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 100.0f, 100.0f});
    auto child_owner = std::make_unique<Node>(2);
    child_owner->set_bounds({10.0f, 10.0f, 20.0f, 20.0f});
    Node& child = root.add_child(std::move(child_owner));

    CountingStyles styles;
    styles.set(1, styled({{"background-color", "#111111"}}));
    styles.set(2, styled({{"background-color", "#ff0000"}}));

    PaintCache cache;
    PaintContext context;
    context.paint_cache = &cache;
    RecordingSink cold;
    paint_scene(root, styles, cold, context);
    clear_all(root);

    child.set_transform(Mat3::rotation(0.5f));
    const PaintCacheStats before = cache.stats();
    RecordingSink rotated;
    paint_scene(root, styles, rotated, context);
    const PaintCacheStats after = cache.stats();

    bool pushed = false;
    for (const Call& call : rotated.calls) {
        if (call.kind == Call::Kind::PushTransform) {
            pushed = true;
        }
    }
    return expect(pushed, "the transformed node replays a transform push") &&
           expect(after.misses - before.misses == 1, "the transformed node misses") &&
           expect(after.hits - before.hits == 1, "the unchanged root still hits");
}

// A parent's clip change invalidates the child's record through the clip-stack fingerprint, even
// though the child's own node state is unchanged.
bool test_inherited_clip_change_misses_child() {
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 100.0f, 100.0f});
    auto child_owner = std::make_unique<Node>(2);
    child_owner->set_bounds({10.0f, 10.0f, 20.0f, 20.0f});
    root.add_child(std::move(child_owner));

    CountingStyles styles;
    styles.set(1, styled({{"background-color", "#111111"},
                          {"overflow-x", "hidden"},
                          {"overflow-y", "hidden"}}));
    styles.set(2, styled({{"background-color", "#ff0000"}}));

    PaintCache cache;
    PaintContext context;
    context.paint_cache = &cache;
    RecordingSink cold;
    paint_scene(root, styles, cold, context);
    clear_all(root);

    // A border changes the painted padding box, so the clip pushed around the child moves.
    styles.set(1, styled({{"background-color", "#111111"},
                          {"overflow-x", "hidden"},
                          {"overflow-y", "hidden"},
                          {"border-top-width", "4px"},
                          {"border-top-style", "solid"},
                          {"border-top-color", "#ffffff"}}));
    root.mark_dirty(Dirty::Paint);
    const PaintCacheStats before = cache.stats();
    styles.calls = 0;
    RecordingSink clipped;
    paint_scene(root, styles, clipped, context);
    const PaintCacheStats after = cache.stats();

    bool new_clip = false;
    for (const Call& call : clipped.calls) {
        if (call.kind == Call::Kind::PushClip && call.clip.rect.y == 4.0f &&
            call.clip.rect.height == 96.0f) {
            new_clip = true;
        }
    }
    return expect(new_clip, "the parent pushes the new padding box") &&
           expect(styles.calls == 2, "parent and child both re-resolve their style") &&
           expect(after.misses - before.misses == 2, "the inherited clip change misses both") &&
           expect(after.hits == before.hits, "no stale child record was replayed");
}

bool test_metrics_source_and_view_bypass() {
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 80.0f, 40.0f});
    PaintData text;
    text.text = "cache text";
    root.set_paint_data(std::move(text));

    CountingStyles styles;
    styles.set(1, styled({{"background-color", "#ff0000"}, {"font-size", "16px"}}));

    PaintCache cache;
    PaintContext stub;
    stub.paint_cache = &cache;
    RecordingSink cold;
    paint_scene(root, styles, cold, stub);
    clear_all(root);

    const PaintCacheStats before_metrics = cache.stats();
    styles.calls = 0;
    FixedMetrics other(kStubTextMetricsId + 7);
    PaintContext real;
    real.paint_cache = &cache;
    real.text_metrics_source = &other;
    RecordingSink real_sink;
    paint_scene(root, styles, real_sink, real);
    const PaintCacheStats after_metrics = cache.stats();

    const bool metrics_missed = expect(after_metrics.misses - before_metrics.misses == 1,
                                       "a different metrics source id misses") &&
                                expect(after_metrics.hits == before_metrics.hits,
                                       "a different metrics source id never hits") &&
                                expect(styles.calls == 1, "the metrics change re-resolves style");

    clear_all(root);
    const PaintCacheStats before_view = cache.stats();
    RecolorView view;
    PaintContext viewed = stub;
    viewed.view = &view;
    RecordingSink with_view;
    paint_scene(root, styles, with_view, viewed);
    RecordingSink uncached_view;
    PaintContext viewed_uncached;
    viewed_uncached.view = &view;
    paint_scene(root, styles, uncached_view, viewed_uncached);
    const PaintCacheStats after_view = cache.stats();

    return metrics_missed &&
           expect(same_calls(with_view.calls, uncached_view.calls),
                  "a paint with a view equals the uncached path") &&
           expect(after_view.hits == before_view.hits, "a view never replays a record") &&
           expect(after_view.bypasses > before_view.bypasses, "the view bypass is counted");
}

} // namespace paint_cache_test

int main() {
    bool ok = true;
    ok = paint_cache_test::test_byte_identical_replay() && ok;
    ok = paint_cache_test::test_clean_subtree_is_not_re_recorded() && ok;
    ok = paint_cache_test::test_dirty_node_re_records_changed_output() && ok;
    ok = paint_cache_test::test_bounds_change_misses() && ok;
    ok = paint_cache_test::test_transform_change_misses() && ok;
    ok = paint_cache_test::test_inherited_clip_change_misses_child() && ok;
    ok = paint_cache_test::test_metrics_source_and_view_bypass() && ok;
    ok = paint_cache_test::test_entry_budget_and_byte_budget() && ok;
    ok = paint_cache_test::test_lru_order() && ok;
    ok = paint_cache_test::test_determinism() && ok;

    if (!ok) {
        std::cerr << "paint_cache: failed\n";
        return 1;
    }
    std::cout << "paint_cache: ok\n";
    return 0;
}
