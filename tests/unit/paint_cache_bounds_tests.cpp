// SPDX-License-Identifier: MIT
//
// Per-node paint cache budgets and determinism: entry/byte caps with deterministic LRU eviction,
// the stats counters, and two identical runs producing identical call sequences and stats. Uses the
// shared fixtures in paint_cache_test_support.hpp; run by the `paint_cache` test executable.

#include "paint_cache_test_support.hpp"

namespace paint_cache_test {

bool test_entry_budget_and_byte_budget() {
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 60.0f, 60.0f});
    auto second_owner = std::make_unique<Node>(2);
    second_owner->set_bounds({0.0f, 0.0f, 10.0f, 10.0f});
    root.add_child(std::move(second_owner));
    auto third_owner = std::make_unique<Node>(3);
    third_owner->set_bounds({20.0f, 0.0f, 10.0f, 10.0f});
    root.add_child(std::move(third_owner));

    CountingStyles styles;
    styles.set(1, styled({{"background-color", "#111111"}}));
    styles.set(2, styled({{"background-color", "#ff0000"}}));
    styles.set(3, styled({{"background-color", "#00ff00"}}));

    PaintCache cache(PaintCacheConfig{2, 1024 * 1024});
    PaintContext context;
    context.paint_cache = &cache;
    RecordingSink first;
    paint_scene(root, styles, first, context);
    PaintCacheStats stats = cache.stats();
    bool ok = expect(stats.entries == 2 && stats.evictions == 1 && stats.misses == 3,
                     "the entry budget evicts down to the cap") &&
              expect(stats.bytes > 0, "entries are byte-accounted");

    clear_all(root);
    const std::uint64_t evictions_before = stats.evictions;
    RecordingSink second;
    paint_scene(root, styles, second, context);
    stats = cache.stats();
    ok = expect(stats.entries == 2, "the cap holds after a second paint") &&
         expect(stats.evictions == evictions_before + 3,
                "the pre-order walk evicts deterministically on every insert") &&
         expect(same_calls(first.calls, second.calls), "eviction does not change the output") && ok;

    // A byte budget smaller than one entry stores nothing instead of overflowing.
    PaintCache tiny(PaintCacheConfig{64, 1});
    PaintContext tiny_context;
    tiny_context.paint_cache = &tiny;
    RecordingSink tiny_sink;
    paint_scene(root, styles, tiny_sink, tiny_context);
    ok = expect(tiny.stats().entries == 0 && tiny.stats().bytes == 0,
                "an entry larger than the byte budget is not stored") &&
         expect(same_calls(first.calls, tiny_sink.calls), "an unstored entry keeps the output") &&
         ok;
    return ok;
}

bool test_lru_order() {
    // Three independent one-node trees share one cache, so the LRU order is observable without the
    // pre-order walk re-recording everything a tree of the same size would.
    Node a(10);
    a.set_bounds({0.0f, 0.0f, 10.0f, 10.0f});
    Node b(11);
    b.set_bounds({0.0f, 0.0f, 10.0f, 10.0f});
    Node c(12);
    c.set_bounds({0.0f, 0.0f, 10.0f, 10.0f});

    CountingStyles styles;
    styles.set(10, styled({{"background-color", "#ff0000"}}));
    styles.set(11, styled({{"background-color", "#00ff00"}}));
    styles.set(12, styled({{"background-color", "#0000ff"}}));

    PaintCache cache(PaintCacheConfig{2, 1024 * 1024});
    PaintContext context;
    context.paint_cache = &cache;

    const auto paint = [&](Node& node) {
        RecordingSink local;
        paint_scene(node, styles, local, context);
        clear_all(node);
        return local.calls;
    };

    const std::vector<Call> a_first = paint(a); // miss: {10}
    (void)paint(b);                             // miss: {10, 11}, 11 most recent
    (void)paint(a);                             // hit: 10 touched, so 11 is now LRU
    (void)paint(c);                             // miss: evicts 11 -> {10, 12}
    const std::vector<Call> a_again = paint(a); // hit: 10 survived the eviction
    (void)paint(b);                             // miss: 11 was evicted
    (void)paint(c);                             // miss: evicting 11 made 12 LRU -> {11, 12}

    const PaintCacheStats stats = cache.stats();
    return expect(!a_first.empty() && same_calls(a_first, a_again),
                  "the surviving node replays byte-identical draws") &&
           expect(stats.hits == 2 && stats.misses == 5 && stats.evictions == 3,
                  "LRU order: the touched node survives, the least recently used is evicted") &&
           expect(stats.entries == 2, "the entry cap holds");
}

bool test_determinism() {
    const auto run = []() {
        Node root(1);
        CountingStyles styles;
        build_rich_scene(root, styles);
        PaintCache cache;
        PaintContext context;
        context.paint_cache = &cache;

        RecordingSink cold;
        paint_scene(root, styles, cold, context);
        clear_all(root);
        RecordingSink warm;
        paint_scene(root, styles, warm, context);

        std::vector<Call> calls = cold.calls;
        calls.insert(calls.end(), warm.calls.begin(), warm.calls.end());
        return std::make_pair(std::move(calls), cache.stats());
    };

    const auto first = run();
    const auto second = run();
    const PaintCacheStats& lhs = first.second;
    const PaintCacheStats& rhs = second.second;
    return expect(same_calls(first.first, second.first),
                  "two identical runs produce identical call sequences") &&
           expect(lhs.hits == rhs.hits && lhs.misses == rhs.misses &&
                      lhs.evictions == rhs.evictions && lhs.invalidations == rhs.invalidations &&
                      lhs.replayed_draws == rhs.replayed_draws &&
                      lhs.recorded_draws == rhs.recorded_draws && lhs.entries == rhs.entries &&
                      lhs.bytes == rhs.bytes,
                  "two identical runs produce identical cache stats");
}

} // namespace paint_cache_test
