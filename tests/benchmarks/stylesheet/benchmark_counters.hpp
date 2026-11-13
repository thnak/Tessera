// SPDX-License-Identifier: MIT
//
// Heap counters for the stylesheet startup benchmark (issue #201). The benchmark compares stage
// counters, never wall-clock time (docs/guides/testing.md section 7), and the allocation count is
// the counter that shows what the blob path saves: loading a prepared sheet decodes bytes, while
// the parse path first tokenizes, parses, expands shorthands, validates values and evaluates
// `@supports`.
//
// The counters are fed by the replaced global operator new/delete in benchmark_counters.cpp, so
// they are exact and deterministic for a given binary: no sampling, no RSS, no clock.

#pragma once

#include <cstddef>

namespace tessera::benchmark {

struct Cost {
    std::size_t allocations = 0;
    std::size_t allocations_bytes = 0;
};

// Process-wide heap totals since start-up.
[[nodiscard]] std::size_t allocation_count() noexcept;
[[nodiscard]] std::size_t allocated_bytes() noexcept;

// Runs `fn` and returns the heap cost it incurred. Deallocations are not counted, so a phase that
// frees more than it allocates still reports every allocation it made.
template<class Fn> Cost measure(Fn&& fn) {
    const std::size_t allocations = allocation_count();
    const std::size_t bytes = allocated_bytes();
    fn();
    return Cost{allocation_count() - allocations, allocated_bytes() - bytes};
}

} // namespace tessera::benchmark
