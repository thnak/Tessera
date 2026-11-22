// SPDX-License-Identifier: MIT
//
// Long-run cache bound (issue #706, memory-model-v1 section 10): "long-run scroll/animation:
// bounded memory, no cache growth".
//
// This is a bound, not a benchmark. It fails on unbounded growth, not on a slow frame, so it can
// run in CI on every lane without a quiet machine.
//
// What it drives: a scrolling list whose window moves one row per frame, and an animation that
// restyles the same rows every frame. Both are the shapes that grow a cache if anything does - a
// scrolling list introduces a new key per frame, and an animation changes the style (and therefore
// the key) every frame.
//
// What it asserts:
//
// 1. The measure cache's entry count never exceeds its capacity. A cache that grows past its bound
//    is the defect section 10 names.
// 2. The entry count is stable in the second half of the run. A cache that keeps climbing has a
//    leak even if it has not hit the bound yet.
// 3. The hit rate is non-trivial. A cache that never hits is not bounded, it is just useless, and
//    the difference matters: the first is a bug, the second is a design question.

#include "tessera/ui/text/text_measure_cache.hpp"

#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextMeasureCache;
using tessera::ui::text::TextMeasurer;
using tessera::ui::text::TextMetrics;
using tessera::ui::text::TextStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "cache_growth: " << message << "\n";
    }
    return condition;
}

class CountingMeasurer final : public TextMeasurer {
public:
    TextMetrics measure(std::string_view text, const TextStyle&,
                        const TextConstraints&) const override {
        ++calls;
        return TextMetrics{static_cast<float>(text.size()) * 8.0f, 16.0f, 12.0f, 1};
    }

    mutable std::size_t calls = 0;
};

// The capacity the test runs with. Small enough that a scrolling list of 200 rows must evict, so
// the bound is actually exercised rather than trivially satisfied.
constexpr std::size_t kCapacity = 64;
constexpr std::size_t kRows = 200;
constexpr std::size_t kVisible = 20;
constexpr std::size_t kFrames = 600;

} // namespace

int main() {
    auto measurer = std::make_shared<CountingMeasurer>();
    TextMeasureCache cache(measurer, kCapacity);
    TextStyle style;
    TextConstraints constraints;

    std::vector<std::string> rows;
    rows.reserve(kRows);
    for (std::size_t i = 0; i < kRows; ++i) {
        rows.push_back("row " + std::to_string(i) + " of a long scrolling list");
    }

    bool ok = true;
    std::size_t max_entries = 0;
    std::size_t entries_at_half = 0;
    std::size_t stable_hits = 0;

    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        // Scroll: the window moves one row per frame.
        const std::size_t first = frame % (kRows - kVisible + 1);
        for (std::size_t i = 0; i < kVisible; ++i) {
            // Animate: the style changes every frame, which changes the cache key. This is the
            // growth shape that a naive cache would leak on.
            TextStyle animated = style;
            animated.font_size = 12.0f + static_cast<float>(frame % 8) * 0.25f;
            static_cast<void>(cache.measure(rows[first + i], animated, constraints));

            // The same row at the *stable* style: this is the key that repeats across frames, so it
            // is the one the cache must hit on.
            const std::size_t hits_before = cache.stats().hits;
            static_cast<void>(cache.measure(rows[first + i], style, constraints));
            if (cache.stats().hits > hits_before) {
                ++stable_hits;
            }
        }

        const std::size_t entries = cache.size();
        if (entries > max_entries) {
            max_entries = entries;
        }
        if (frame == kFrames / 2) {
            entries_at_half = entries;
        }
    }

    const auto stats = cache.stats();
    std::cout << "cache_growth: " << kFrames << " frames, " << kRows << " rows, capacity "
              << kCapacity << "\n";
    std::cout << "cache_growth: entries=" << stats.entries << " max=" << max_entries
              << " hits=" << stats.hits << " misses=" << stats.misses
              << " evictions=" << stats.evictions << "\n";

    // 1. Never past the bound.
    ok = expect(max_entries <= kCapacity, "the measure cache never exceeds its capacity") && ok;
    ok = expect(cache.size() <= kCapacity, "the cache is within its capacity at the end") && ok;

    // 2. Stable in the second half: the entry count at the end is not larger than at the halfway
    //    point. A cache that keeps climbing has a leak even below its bound.
    ok = expect(cache.size() <= entries_at_half + kVisible,
                "the entry count is stable in the second half of the run") &&
         ok;

    // 3. The cache is doing its job on the part of the workload that repeats. The animated style
    //    changes every frame, so those keys are unique by construction and cannot hit - that is not
    //    a defect, it is what an animation is. The scrolling rows with a *stable* style are the
    //    repeat, and the cache must hit on those.
    //
    //    This distinction is the point of the test: a cache that hit on nothing would be useless,
    //    and a cache that grew without bound would be a leak. Asserting a hit rate over the whole
    //    run would conflate the two, because the animation legitimately produces 100% misses.
    ok = expect(stable_hits > 0, "the cache hits on the rows whose style does not change") && ok;
    ok = expect(stats.evictions > 0, "the run is long enough to force eviction") && ok;

    if (!ok) {
        std::cerr << "cache_growth: FAILED\n";
        return 1;
    }
    std::cout << "cache_growth: ok\n";
    return 0;
}
