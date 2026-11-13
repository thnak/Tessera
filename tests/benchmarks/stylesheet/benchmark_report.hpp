// SPDX-License-Identifier: MIT
//
// Table printing and baseline comparison for the stylesheet startup benchmark (issue #201). The
// benchmark reports counters, not timings: the interesting number is how much prepare work the blob
// path removes, and every number here is a count that a second run reproduces exactly.

#pragma once

#include "benchmark_baseline.hpp"
#include "benchmark_paths.hpp"

namespace tessera::benchmark {

// Prints the two-column counter table and the one-line saving summary.
void print_table(const PathComparison& comparison);

// Compares the measured counters with the committed baseline: structural counters must match
// exactly, allocation counters may drift by `allocation_percent`. Returns false, with one line per
// mismatch on stderr, when the measured run is outside the baseline.
[[nodiscard]] bool compare(const PathComparison& comparison, const Baseline& baseline);

} // namespace tessera::benchmark
