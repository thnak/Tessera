// SPDX-License-Identifier: MIT
//
// Budget and summary arithmetic for the CPU benchmark (issue #241). The committed budget
// (`tests/golden/perf/cpu_benchmark.txt`) is deliberately generous: it detects an
// order-of-magnitude regression on a slow CI runner, it does not rank machines. Times are compared
// on the median so a shared, noisy runner cannot fail the test on one outlier; p95 is reported for
// the trend.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tessera::benchmark {

// One committed budget. `max_*_ms` values are integer milliseconds; the counters are exact
// draw-list/redraw counts with headroom (see the file's own comments).
struct Budget {
    std::uint64_t nodes = 0;
    std::uint64_t runs = 0;
    std::uint64_t warmup_runs = 0;
    std::uint64_t max_first_style_layout_ms = 0;
    std::uint64_t max_full_paint_ms = 0;
    std::uint64_t max_layout_paint_text_ms = 0;
    std::uint64_t max_layout_paint_container_ms = 0;
    std::uint64_t max_damage_rects = 0;
    std::uint64_t max_draw_calls = 0;
    std::uint64_t max_quads = 0;
    std::uint64_t max_primitives = 0;
};

// Reads `key = value` lines; '#' starts a comment. False when a required key is missing or a value
// is not a positive integer.
[[nodiscard]] bool load_budget(const char* path, Budget& out);

struct Percentiles {
    double median = 0.0;
    double p95 = 0.0;
};

// Sorts a copy. The median is the middle value (mean of the two middles for an even count); p95 is
// linearly interpolated at index `0.95 * (n - 1)` so it stays meaningful for small sample counts.
[[nodiscard]] Percentiles summarize(std::vector<double> samples);

} // namespace tessera::benchmark
