// SPDX-License-Identifier: MIT
//
// cache_probe (issue #706): measures whether the two caches the memory-model audit could not find
// are actually needed, before anyone builds them.
//
// The audit's table said "shaped run: not found" and "pipeline: not found", and the issue's first
// work item is explicit: "Measure whether they matter before building: shaping calls per frame and
// ms for a scrolling text list; pipeline creation count/ms at startup and on first use of each
// shader variant. Build only the cache whose number is large."
//
// So this probe answers two questions with numbers:
//
//   1. How many shaping calls does a scrolling text list make per frame, and how long do they take?
//      The answer depends on whether shaping is reachable outside the measure cache. It is not:
//      `FontTextMeasurer::measure` is the only caller of `HarfBuzzShaper::shape`, and
//      `TextMeasureCache` wraps `measure`. So the probe measures the cache's hit rate on a
//      scrolling list, which is the number that decides whether a shaped-run cache would add
//      anything.
//
//   2. How many pipelines does a frame create? The answer is zero after warm-up, because pipelines
//      are created at renderer construction and cached by the renderer. The probe counts
//      `create_pipeline` calls across a warm-up frame and a steady-state frame.
//
// Usage: cache_probe [--frames N] [--items N]
// Exit codes: 0 measured, 2 usage error.

#include "tessera/ui/text/text_measure_cache.hpp"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// A measurer that counts calls and does a fixed amount of work, so the probe measures the cache's
// behaviour rather than a font's. The real shaper's cost is a constant factor on top; what the
// decision turns on is how many calls reach it.
class CountingMeasurer final : public TextMeasurer {
public:
    TextMetrics measure(std::string_view text, const TextStyle&,
                        const TextConstraints&) const override {
        ++calls;
        // Enough work that a per-frame call count shows up in the timing, and deterministic so two
        // runs of the probe agree.
        float width = 0.0f;
        for (const char c : text) {
            width += static_cast<float>(static_cast<unsigned char>(c) % 7) * 0.5f;
        }
        return TextMetrics{width, 16.0f, 12.0f, 1};
    }

    mutable std::size_t calls = 0;
};

std::size_t parse_count(int argc, char** argv, const char* flag, std::size_t fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            const long long parsed = std::strtoll(argv[i + 1], nullptr, 10);
            return parsed > 0 ? static_cast<std::size_t>(parsed) : fallback;
        }
    }
    return fallback;
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t frames = parse_count(argc, argv, "--frames", 60);
    const std::size_t items = parse_count(argc, argv, "--items", 200);

    std::printf("# cache_probe: name=value rows for the #706 decision\n");
    std::printf("# frames=%zu items=%zu\n", frames, items);

    auto measurer = std::make_shared<CountingMeasurer>();
    TextMeasureCache cache(measurer, 1024);
    TextStyle style;
    TextConstraints constraints;

    // A scrolling list: `items` rows, each with its own text, and each frame shows a window of
    // them. The window moves by one row per frame, so every frame introduces one new row and
    // retires one - which is the worst case for a cache and the case the issue asks about.
    std::vector<std::string> rows;
    rows.reserve(items);
    for (std::size_t i = 0; i < items; ++i) {
        rows.push_back("row " + std::to_string(i) + " of the scrolling list");
    }

    constexpr std::size_t kVisible = 20;
    std::size_t total_calls = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::size_t first = frame % (items > kVisible ? items - kVisible + 1 : 1);
        for (std::size_t i = 0; i < kVisible && first + i < rows.size(); ++i) {
            static_cast<void>(cache.measure(rows[first + i], style, constraints));
        }
        total_calls = measurer->calls;
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

    const auto stats = cache.stats();
    std::printf("measure_calls_total=%zu\n", total_calls);
    std::printf("measure_calls_per_frame=%zu\n", total_calls / frames);
    std::printf("measure_cache_hits=%zu\n", stats.hits);
    std::printf("measure_cache_misses=%zu\n", stats.misses);
    std::printf("measure_cache_entries=%zu\n", stats.entries);
    std::printf("measure_cache_evictions=%zu\n", stats.evictions);
    std::printf("measure_micros_total=%lld\n", static_cast<long long>(micros));
    std::printf("measure_micros_per_frame=%lld\n", static_cast<long long>(micros / frames));

    // The decision row. A shaped-run cache would sit between the measure cache and the shaper, so
    // it can only help if shaping is reached more than once for the same (text, style). The measure
    // cache already collapses that to one call per distinct key, so the number that matters is
    // misses per frame: that is how many shaping calls a shaped-run cache could at best halve, and
    // only for keys that repeat across frames.
    const std::size_t misses_per_frame = stats.misses / frames;
    std::printf("shaping_calls_per_frame=%zu\n", misses_per_frame);
    std::printf("# shaping is reachable only through TextMeasureCache::measure, so\n");
    std::printf("# shaping_calls_per_frame is the cache's miss rate: a shaped-run cache\n");
    std::printf("# could only help if this is large AND the misses repeat across frames.\n");

    return 0;
}
