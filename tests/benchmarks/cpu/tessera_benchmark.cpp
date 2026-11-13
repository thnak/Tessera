// SPDX-License-Identifier: MIT
//
// tessera_benchmark: the GPU-free CPU benchmark of issue #241. It drives a headless frame loop over
// the deterministic 12 000-node reference scene (style, Yoga layout, scene-style sync, damage,
// paint), records every paint command into draw lists through the null RHI, and prints a table that
// CTest's log keeps for the trend. Times are median/p95 over N runs after warm-up; the committed
// budget in `tests/golden/perf/cpu_benchmark.txt` is an order-of-magnitude detector, not a speed
// ranking.
//
// No GPU, no window, no font, no wall-clock-dependent output: the text measurer is the engine's
// deterministic stub, the render target and pipeline are null-RHI objects, and the only clock used
// is the measurement clock itself.

#include "benchmark_budget.hpp"
#include "benchmark_fixture.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace benchmark = tessera::benchmark;

// Trigger of docs/design/performance.md section 9.1: revisit the single-threaded decision when
// paint exceeds about 4 ms or 30% of the 60 Hz frame budget at 10k nodes.
constexpr double kPaintTriggerMs = 4.0;
constexpr double kFrameBudgetMs = 1000.0 / 60.0;

[[nodiscard]] bool within(const char* what, double value, std::uint64_t budget) {
    if (value <= static_cast<double>(budget)) {
        return true;
    }
    std::fprintf(stderr, "cpu_benchmark: %s %.1f exceeds the committed budget %llu\n", what, value,
                 static_cast<unsigned long long>(budget));
    return false;
}

[[nodiscard]] bool within(const char* what, std::size_t value, std::uint64_t budget) {
    return within(what, static_cast<double>(value), budget);
}

[[nodiscard]] const char* compiler_name() {
#if defined(_MSC_VER)
    return "MSVC";
#elif defined(__clang__)
    return "Clang";
#elif defined(__GNUC__)
    return "GCC";
#else
    return "unknown compiler";
#endif
}

[[nodiscard]] const char* platform_name() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__linux__)
    return "Linux";
#else
    return "unknown platform";
#endif
}

[[nodiscard]] const char* build_name() {
#if defined(NDEBUG)
    return "Release";
#else
    return "Debug";
#endif
}

// Accumulates the per-run counters (deterministic, so the maximum is the reported value).
void accumulate(benchmark::Totals& totals, const benchmark::Measurement& sample) {
    totals.damage_rects = std::max({totals.damage_rects, sample.damage_rects_full,
                                    sample.damage_rects_text, sample.damage_rects_container});
    totals.draw_calls = std::max(totals.draw_calls, sample.draw_calls);
    totals.quads = std::max(totals.quads, sample.quads);
    totals.primitives = std::max(totals.primitives, sample.primitives);
    totals.text_runs = std::max(totals.text_runs, sample.text_runs);
    totals.images = std::max(totals.images, sample.images);
    totals.paint_nodes = std::max(totals.paint_nodes, sample.paint_nodes);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: tessera_benchmark <tests/golden/perf/cpu_benchmark.txt>\n");
        return 2;
    }
    benchmark::Budget budget;
    if (!benchmark::load_budget(argv[1], budget)) {
        std::fprintf(stderr, "cpu_benchmark: cannot read the budget %s\n", argv[1]);
        return 2;
    }
    if (budget.nodes != benchmark::kBenchmarkNodes) {
        std::fprintf(stderr, "cpu_benchmark: the budget expects %llu nodes, the scene builds %zu\n",
                     static_cast<unsigned long long>(budget.nodes), benchmark::kBenchmarkNodes);
        return 2;
    }

    std::printf("cpu_benchmark: scene %zu nodes, %gx%g logical px @1x, null RHI, text stub\n",
                benchmark::kBenchmarkNodes, static_cast<double>(benchmark::kBenchmarkWidth),
                static_cast<double>(benchmark::kBenchmarkHeight));
    std::printf("cpu_benchmark: machine %s, %u logical cores, %s, %s\n", platform_name(),
                std::thread::hardware_concurrency(), compiler_name(), build_name());
    std::printf("cpu_benchmark: %llu measured runs after %llu warm-up runs\n",
                static_cast<unsigned long long>(budget.runs),
                static_cast<unsigned long long>(budget.warmup_runs));

    for (std::uint64_t run = 0; run < budget.warmup_runs; ++run) {
        benchmark::Fixture fixture;
        (void)fixture.measure();
    }

    std::vector<double> first_style_layout;
    std::vector<double> full_paint;
    std::vector<double> layout_paint_text;
    std::vector<double> layout_paint_container;
    benchmark::Totals totals;
    for (std::uint64_t run = 0; run < budget.runs; ++run) {
        benchmark::Fixture fixture;
        if (fixture.nodes() != benchmark::kBenchmarkNodes) {
            std::fprintf(stderr, "cpu_benchmark: the scene built %zu nodes, expected %zu\n",
                         fixture.nodes(), benchmark::kBenchmarkNodes);
            return 1;
        }
        const benchmark::Measurement sample = fixture.measure();
        first_style_layout.push_back(sample.first_style_layout_ms);
        full_paint.push_back(sample.full_paint_ms);
        layout_paint_text.push_back(sample.layout_paint_text_ms);
        layout_paint_container.push_back(sample.layout_paint_container_ms);
        accumulate(totals, sample);
    }
    const benchmark::Percentiles first = benchmark::summarize(std::move(first_style_layout));
    const benchmark::Percentiles paint = benchmark::summarize(std::move(full_paint));
    const benchmark::Percentiles text = benchmark::summarize(std::move(layout_paint_text));
    const benchmark::Percentiles container =
        benchmark::summarize(std::move(layout_paint_container));

    std::printf("cpu_benchmark: metric                              median      p95    budget\n");
    std::printf("cpu_benchmark: first style+layout (ms)       %10.2f %8.2f %8llu\n", first.median,
                first.p95, static_cast<unsigned long long>(budget.max_first_style_layout_ms));
    std::printf("cpu_benchmark: full paint to draw lists (ms) %10.2f %8.2f %8llu\n", paint.median,
                paint.p95, static_cast<unsigned long long>(budget.max_full_paint_ms));
    std::printf("cpu_benchmark: layout+paint, one text (ms)   %10.2f %8.2f %8llu\n", text.median,
                text.p95, static_cast<unsigned long long>(budget.max_layout_paint_text_ms));
    std::printf("cpu_benchmark: layout+paint, one size (ms)   %10.2f %8.2f %8llu\n",
                container.median, container.p95,
                static_cast<unsigned long long>(budget.max_layout_paint_container_ms));
    std::printf("cpu_benchmark: damage rects (max of the three frames) %zu (budget %llu)\n",
                totals.damage_rects, static_cast<unsigned long long>(budget.max_damage_rects));
    std::printf("cpu_benchmark: draw calls %zu, quads %zu, draw-list primitives %zu\n",
                totals.draw_calls, totals.quads, totals.primitives);
    std::printf("cpu_benchmark: text runs %zu (stub metrics, not rasterized), image draws %zu\n",
                totals.text_runs, totals.images);
    std::printf("cpu_benchmark: nodes painted by the full pass %zu\n", totals.paint_nodes);

    const double trigger_ms = std::min(kPaintTriggerMs, kFrameBudgetMs * 0.30);
    std::printf("cpu_benchmark: paint trigger: median %.2f ms vs %.2f ms -> %s\n", paint.median,
                trigger_ms,
                paint.median > trigger_ms ? "TRIGGERED (record on issue #241)" : "not triggered");

    bool ok = true;
    ok = within("first style+layout ms", first.median, budget.max_first_style_layout_ms) && ok;
    ok = within("full paint ms", paint.median, budget.max_full_paint_ms) && ok;
    ok = within("layout+paint text ms", text.median, budget.max_layout_paint_text_ms) && ok;
    ok = within("layout+paint container ms", container.median,
                budget.max_layout_paint_container_ms) &&
         ok;
    ok = within("damage rects", totals.damage_rects, budget.max_damage_rects) && ok;
    ok = within("draw calls", totals.draw_calls, budget.max_draw_calls) && ok;
    ok = within("quads", totals.quads, budget.max_quads) && ok;
    ok = within("draw-list primitives", totals.primitives, budget.max_primitives) && ok;
    if (!ok) {
        return 1;
    }
    std::printf("cpu_benchmark: ok\n");
    return 0;
}
