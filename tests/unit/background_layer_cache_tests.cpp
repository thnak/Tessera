// SPDX-License-Identifier: MIT
//
// The per-style `background-image` parse cache (issue #733 item 2). What it must prove:
//
//   1. A second read of the same style hits, and a *different* style that holds the same value
//      text is a distinct entry -- the key is style identity, not the text.
//   2. A style whose entries are mutated afterwards cannot serve a stale parse: the copy-on-write
//      storage hands it a new identity, so the old entry is simply not found.
//   3. An invalid value is cached as "does not parse" and is not retried.
//   4. The cache is bounded in entries and bytes, evicts least-recently-used first, and keeps the
//      answer correct when a value does not fit the byte budget at all.
//   5. `enabled(false)` bypasses the cache (miss-only), and `invalidate`/`clear` drop everything.
//
// The paint-level half asserts the observable consequence: a scene inside one paint pass parses
// each distinct style once, so the miss count is the number of distinct styles, not the number of
// nodes.

#include "tessera/ui/paint/background_layer_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace {

using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::BackgroundLayerCache;
using tessera::ui::paint::BackgroundLayerCacheConfig;
using tessera::ui::paint::BackgroundLayerCacheStats;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::BackgroundLayerList;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "background_layer_cache: " << message << "\n";
    }
    return condition;
}

constexpr const char* kGradient = "linear-gradient(180deg, #ff0000 0%, #0000ff 100%)";
constexpr const char* kOtherGradient = "linear-gradient(90deg, #00ff00 0%, #000000 100%)";

ComputedStyle image_style(const std::string& value) {
    ComputedStyle style;
    style.set("background-image", value);
    return style;
}

// 1. Identity is the key: one style hits, two styles holding the same text do not share an entry.
bool identity_is_the_key() {
    bool ok = true;
    BackgroundLayerCache cache;
    const ComputedStyle first = image_style(kGradient);

    const auto& a = cache.get(first, kGradient);
    ok = expect(a.parsed.has_value(), "a valid value parses") && ok;
    BackgroundLayerCacheStats stats = cache.stats();
    ok = expect(stats.misses == 1 && stats.hits == 0, "the first read misses") && ok;

    const auto& b = cache.get(first, kGradient);
    stats = cache.stats();
    ok = expect(stats.hits == 1, "the same style hits the second time") && ok;
    ok = expect(a.parsed->count == b.parsed->count, "a hit returns the same value") && ok;

    // A copy of the style shares the entries block, so it is the same identity and the same entry.
    const ComputedStyle copy = first;
    (void)cache.get(copy, kGradient);
    stats = cache.stats();
    ok = expect(stats.hits == 2, "a copy of the style shares its identity and hits") && ok;
    ok = expect(stats.entries == 1, "a copy does not add a second entry") && ok;

    // A *different* style with the same text is a different identity: its own entry.
    const ComputedStyle second = image_style(kGradient);
    const auto& c = cache.get(second, kGradient);
    stats = cache.stats();
    ok = expect(stats.misses == 2, "a different style with the same text is a new entry") && ok;
    ok = expect(stats.entries == 2, "two identities hold two entries") && ok;
    ok = expect(c.parsed.has_value() && c.parsed->count == a.parsed->count,
                "the second style parses to the same list") &&
         ok;
    return ok;
}

// 2. A restyle that mutates the entries gets a new identity, so the old parse is never served.
bool mutation_changes_identity() {
    bool ok = true;
    BackgroundLayerCache cache;
    ComputedStyle style = image_style(kGradient);
    (void)cache.get(style, kGradient);
    ok = expect(cache.stats().entries == 1, "the first value is cached") && ok;

    // Copy-on-write: the shared block is replaced, so the identity the entry was keyed by is gone.
    style.set("background-image", kOtherGradient);
    const auto& after = cache.get(style, kOtherGradient);
    const BackgroundLayerCacheStats stats = cache.stats();
    ok = expect(stats.misses == 2, "a mutated style misses instead of serving the old parse") && ok;
    ok = expect(stats.entries == 2, "the mutated style gets its own entry") && ok;
    ok = expect(after.parsed.has_value(), "the new value still parses") && ok;

    // The two parses must actually differ, or the test above proves nothing about staleness.
    const auto old = cache.get(image_style(kGradient), kGradient);
    ok = expect(old.parsed.has_value(), "the old value still parses on its own identity") && ok;
    ok = expect(after.parsed->layers[0].gradient.linear.angle_degrees !=
                    old.parsed->layers[0].gradient.linear.angle_degrees,
                "the two parsed values differ, so a stale hit would be observable") &&
         ok;
    return ok;
}

// 3. An invalid value is cached as "does not parse", not retried on every read.
bool invalid_values_are_cached() {
    bool ok = true;
    BackgroundLayerCache cache;
    const ComputedStyle style = image_style("url(a.png)");

    const auto& first = cache.get(style, "url(a.png)");
    ok = expect(!first.parsed.has_value(), "a url() layer does not parse") && ok;
    const auto& second = cache.get(style, "url(a.png)");
    ok = expect(!second.parsed.has_value(), "the cached answer is still 'does not parse'") && ok;
    const BackgroundLayerCacheStats stats = cache.stats();
    ok = expect(stats.misses == 1 && stats.hits == 1, "the invalid value is cached and hit") && ok;
    return ok;
}

// 4. The entry budget evicts least-recently-used first and keeps answers correct.
bool entry_budget_evicts_lru() {
    bool ok = true;
    BackgroundLayerCacheConfig config;
    config.max_entries = 2;
    BackgroundLayerCache cache(config);

    const ComputedStyle a = image_style(kGradient);
    const ComputedStyle b = image_style(kOtherGradient);
    const ComputedStyle c = image_style("linear-gradient(0deg, #111111, #222222)");

    (void)cache.get(a, kGradient);
    (void)cache.get(b, kOtherGradient);
    // Touch `a` so `b` becomes the least recently used.
    (void)cache.get(a, kGradient);
    (void)cache.get(c, "linear-gradient(0deg, #111111, #222222)");

    BackgroundLayerCacheStats stats = cache.stats();
    ok = expect(stats.entries == 2, "the entry budget is enforced") && ok;
    ok = expect(stats.evictions == 1, "one entry was evicted") && ok;

    // `a` was touched, so it survived: reading it hits.
    const std::uint64_t hits_before = cache.stats().hits;
    (void)cache.get(a, kGradient);
    ok = expect(cache.stats().hits == hits_before + 1, "the recently used entry survived") && ok;

    // `b` was the victim: reading it misses and re-inserts.
    const std::uint64_t misses_before = cache.stats().misses;
    const auto& revived = cache.get(b, kOtherGradient);
    stats = cache.stats();
    ok = expect(stats.misses == misses_before + 1, "the least recently used entry was evicted") &&
         ok;
    ok = expect(revived.parsed.has_value(), "a revived value still parses") && ok;
    return ok;
}

// 4b. A budget too small for one entry still answers correctly, through scratch.
bool byte_budget_falls_back_to_scratch() {
    bool ok = true;
    BackgroundLayerCacheConfig config;
    config.max_bytes = 1; // smaller than a single entry
    BackgroundLayerCache cache(config);

    const ComputedStyle style = image_style(kGradient);
    const auto& first = cache.get(style, kGradient);
    ok = expect(first.parsed.has_value(), "the value parses even when it cannot be stored") && ok;
    const auto& second = cache.get(style, kGradient);
    ok = expect(second.parsed.has_value(), "the second read parses again") && ok;
    const BackgroundLayerCacheStats stats = cache.stats();
    ok = expect(stats.entries == 0, "nothing was stored") && ok;
    ok = expect(stats.misses == 2, "both reads missed") && ok;
    return ok;
}

// 5. The bypass and the reset paths.
bool bypass_and_reset() {
    bool ok = true;
    BackgroundLayerCache cache;
    const ComputedStyle style = image_style(kGradient);

    cache.set_enabled(false);
    ok = expect(!cache.enabled(), "the cache reports disabled") && ok;
    (void)cache.get(style, kGradient);
    (void)cache.get(style, kGradient);
    BackgroundLayerCacheStats stats = cache.stats();
    ok = expect(stats.entries == 0, "a disabled cache stores nothing") && ok;
    ok = expect(stats.misses == 2 && stats.hits == 0, "a disabled cache misses every read") && ok;
    // The answer is still correct on the bypass path.
    const auto& bypassed = cache.get(style, kGradient);
    ok = expect(bypassed.parsed.has_value(), "the bypass path still parses") && ok;

    cache.set_enabled(true);
    (void)cache.get(style, kGradient);
    ok = expect(cache.stats().entries == 1, "re-enabling caches again") && ok;

    cache.invalidate();
    stats = cache.stats();
    ok = expect(stats.entries == 0 && stats.invalidations == 1, "invalidate drops every entry") &&
         ok;

    (void)cache.get(style, kGradient);
    cache.clear();
    ok = expect(cache.stats().entries == 0, "clear drops every entry") && ok;

    // A default-constructed style has no entries, hence no identity: it is never cached, so one
    // empty style's value can never be served to another.
    BackgroundLayerCache plain_cache;
    const ComputedStyle empty;
    (void)plain_cache.get(empty, kGradient);
    ok = expect(plain_cache.stats().entries == 0, "a style with no entries is not cached") && ok;
    return ok;
}

// A recording sink so the paint-level test can count what reached the sink.
class CountingSink final : public PaintSink {
public:
    void fill_rect(const tessera::rendering::rhi::Rect&,
                   const tessera::rendering::rhi::Color&) override {}
    void fill_rounded_rect(const tessera::rendering::rhi::Rect&,
                           const tessera::rendering::rhi::Color&, float) override {}
    void fill_linear_gradient(const tessera::ui::paint::GradientDraw&) override {
        ++gradients;
    }
    void fill_radial_gradient(const tessera::ui::paint::RadialGradientDraw&) override {
        ++gradients;
    }
    void fill_conic_gradient(const tessera::ui::paint::ConicGradientDraw&) override {
        ++gradients;
    }
    int gradients = 0;
};

// A provider that hands out one style per node id from a fixed table. `compute` is the pure virtual
// the base declares; the paint pass only calls `resolve`, so this forwards to the same table.
class FixedStyles final : public StyleProvider {
public:
    std::map<NodeId, ComputedStyle> styles;
    ComputedStyle compute(const Node& node) override {
        return resolve(node);
    }
    const ComputedStyle& resolve(const Node& node) override {
        const auto found = styles.find(node.id());
        return found == styles.end() ? empty_ : found->second;
    }

private:
    ComputedStyle empty_;
};

// Builds a flat root with `count` equally sized children, in document order, with explicit ids.
// A `Node` built without an id gets 0, which no style table entry can match, so every id here is
// assigned: the root is 1 and the children follow.
Node& add_children(Node& root, int count, float width) {
    for (int index = 0; index < count; ++index) {
        auto child = std::make_unique<Node>(static_cast<NodeId>(index) + 2);
        child->set_bounds(
            tessera::rendering::rhi::Rect{static_cast<float>(index) * width, 0.0f, width, 100.0f});
        root.add_child(std::move(child));
    }
    return root;
}

// 6. Ends to end: one parse per distinct style across a whole paint, not one per node.
bool one_parse_per_style_per_paint() {
    bool ok = true;
    // Six painted children, but only two distinct styles: three nodes get a copy of the first
    // template and three a copy of the second. Copies share their entries block, so the paint pass
    // sees two identities, not six.
    const ComputedStyle first = image_style(kGradient);
    const ComputedStyle second = image_style(kOtherGradient);

    Node root(1);
    root.set_bounds(tessera::rendering::rhi::Rect{0.0f, 0.0f, 600.0f, 100.0f});
    add_children(root, 6, 100.0f);

    FixedStyles provider;
    for (int index = 0; index < 6; ++index) {
        provider.styles[static_cast<NodeId>(index) + 2] = index < 3 ? first : second;
    }

    BackgroundLayerCache cache;
    PaintContext context;
    context.background_layers = &cache;
    CountingSink sink;
    (void)paint_scene(root, provider, sink, context);

    ok =
        expect(sink.gradients == 6, "every node with a background-image still paints a gradient") &&
        ok;
    const BackgroundLayerCacheStats cache_stats = cache.stats();
    ok = expect(cache_stats.misses == 2,
                "six nodes sharing two style identities parse twice, not six times") &&
         ok;

    // A second paint of the same scene parses nothing: every style identity is already known.
    CountingSink second_sink;
    (void)paint_scene(root, provider, second_sink, context);
    const BackgroundLayerCacheStats after = cache.stats();
    ok = expect(second_sink.gradients == 6, "the second paint draws the same gradients") && ok;
    ok = expect(after.misses == cache_stats.misses, "the second paint parses nothing") && ok;
    ok = expect(after.hits >= 6, "the second paint hits once per painted node") && ok;
    return ok;
}

// 7. A null cache is the uncached path: identical output, nothing left behind.
bool null_cache_matches_the_uncached_path() {
    bool ok = true;
    Node root(1);
    root.set_bounds(tessera::rendering::rhi::Rect{0.0f, 0.0f, 200.0f, 100.0f});
    add_children(root, 2, 100.0f);

    const ComputedStyle first = image_style(kGradient);
    const ComputedStyle second = image_style(kOtherGradient);
    FixedStyles provider;
    provider.styles[2] = first;
    provider.styles[3] = second;

    PaintContext uncached;
    CountingSink uncached_sink;
    (void)paint_scene(root, provider, uncached_sink, uncached);

    BackgroundLayerCache cache;
    PaintContext cached;
    cached.background_layers = &cache;
    CountingSink cached_sink;
    (void)paint_scene(root, provider, cached_sink, cached);

    ok = expect(uncached_sink.gradients == cached_sink.gradients,
                "the cached path emits the same draws as the uncached one") &&
         ok;
    ok = expect(uncached_sink.gradients == 2, "both nodes drew a gradient") && ok;
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = identity_is_the_key() && ok;
    ok = mutation_changes_identity() && ok;
    ok = invalid_values_are_cached() && ok;
    ok = entry_budget_evicts_lru() && ok;
    ok = byte_budget_falls_back_to_scratch() && ok;
    ok = bypass_and_reset() && ok;
    ok = one_parse_per_style_per_paint() && ok;
    ok = null_cache_matches_the_uncached_path() && ok;

    if (!ok) {
        std::cerr << "background_layer_cache: FAILED\n";
        return 1;
    }
    std::cout << "background_layer_cache: all checks passed\n";
    return 0;
}
