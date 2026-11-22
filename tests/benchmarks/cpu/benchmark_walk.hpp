// SPDX-License-Identifier: MIT
//
// Scene-walk microbenchmark: the baseline for the Node cache-line work (docs/architecture/
// god-type-audit-v1.md section 3.1). It times the three passes that visit every node of the 12 000
// node reference scene after the first frame has laid it out: a plain hot-field walk (the lower
// bound a traversal can reach), the dirty pass, and hit testing over a fixed grid of points. On
// Linux it also reads the hardware L1D read-miss, cache-miss and instruction counters around each
// phase; elsewhere, or where perf events are not permitted, those columns print "n/a".
//
// It is a report, not a gate: no budget file, no failing exit, so it cannot flake CI. Run it as
// `tessera_benchmark --walk [rounds]`.

#pragma once

namespace tessera::benchmark {

// Returns the process exit code.
[[nodiscard]] int run_scene_walk(int rounds);

} // namespace tessera::benchmark
