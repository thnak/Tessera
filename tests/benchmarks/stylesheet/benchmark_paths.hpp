// SPDX-License-Identifier: MIT
//
// The two stylesheet startup paths of ADR-048, measured on the same stylesheet (issue #201):
//
//   blob  : the bytes codegen embeds -- serialize_stylesheet(prepare_stylesheet(css)) -- decoded by
//           load_stylesheet_blob() and resolved into a CompiledStyleSheet. No tokenizer, no parser.
//   parse : compile_stylesheet(), i.e. prepare_stylesheet() (tokenize, parse selectors, expand
//           shorthands, validate values, evaluate @supports) followed by the same resolve.
//
// The resolve stage is shared: both paths hand the same PreparedStyleSheet to the same
// CompiledStyleSheet constructor, so the difference between the two columns is exactly the prepare
// stage the blob path replaces. The blob bytes themselves are produced once, before any
// measurement: that is the build-time step codegen performs, not startup work.

#pragma once

#include "benchmark_counters.hpp"
#include "tessera/ui/style/media_query.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace tessera::benchmark {

// Counters of one startup path. `prepare` is the path-specific stage (blob decode or CSS prepare)
// and `resolve` is the shared stage that turns a PreparedStyleSheet into a CompiledStyleSheet.
struct PathCounters {
    std::size_t bytes_read = 0;
    Cost prepare;
    Cost resolve;
    // Deterministic fingerprint of the compiled sheet: equal values mean the two paths produced the
    // same cascade, which is what makes the counter comparison meaningful.
    std::uint64_t fingerprint = 0;
    std::size_t rules = 0;
    std::size_t selectors = 0;
    std::size_t declarations = 0;
    std::size_t diagnostics = 0;
    std::size_t keyframes = 0;
    std::size_t layers = 0;
    std::size_t container_queries = 0;
};

struct PathComparison {
    PathCounters blob;
    PathCounters parse;
    std::size_t css_bytes = 0;
    std::size_t css_tokens = 0;
    std::size_t blob_bytes = 0;
};

// Runs both paths twice. The first run is reported and the second must reproduce every counter
// exactly, which is the determinism check: no clock, no random source, no filesystem ordering.
// Returns nullopt when the blob does not load or the counters are not reproducible.
[[nodiscard]] std::optional<PathComparison> run_paths(std::string_view css);

// True when every compiled-sheet counter and the fingerprint agree between the two paths.
[[nodiscard]] bool same_cascade(const PathComparison& comparison);

} // namespace tessera::benchmark
