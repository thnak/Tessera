// SPDX-License-Identifier: MIT

#include "benchmark_report.hpp"

#include <cmath>
#include <cstdio>

namespace tessera::benchmark {
namespace {

void row(const char* label, std::size_t blob, std::size_t parse) {
    const auto saved = static_cast<long long>(parse) - static_cast<long long>(blob);
    std::printf("stylesheet_benchmark: %-44s%12zu%12zu", label, blob, parse);
    if (saved == 0) {
        std::printf("%13s\n", "0");
    } else {
        std::printf("%+13lld\n", saved);
    }
}

} // namespace

void print_table(const PathComparison& comparison) {
    std::printf(
        "stylesheet_benchmark: one stylesheet, two startup paths; counters only, no wall clock\n");
    std::printf("stylesheet_benchmark: stylesheet %zu bytes CSS, %zu tokens; blob %zu bytes\n",
                comparison.css_bytes, comparison.css_tokens, comparison.blob_bytes);
    std::printf("stylesheet_benchmark: %-44s%12s%12s%13s\n", "stage counter", "blob", "parse",
                "parse-blob");
    row("input bytes read", comparison.blob.bytes_read, comparison.parse.bytes_read);
    row("prepare allocations", comparison.blob.prepare.allocations,
        comparison.parse.prepare.allocations);
    row("prepare allocation bytes", comparison.blob.prepare.allocations_bytes,
        comparison.parse.prepare.allocations_bytes);
    row("resolve allocations (shared)", comparison.blob.resolve.allocations,
        comparison.parse.resolve.allocations);
    row("resolve allocation bytes (shared)", comparison.blob.resolve.allocations_bytes,
        comparison.parse.resolve.allocations_bytes);
    row("rules", comparison.blob.rules, comparison.parse.rules);
    row("selectors", comparison.blob.selectors, comparison.parse.selectors);
    row("declarations", comparison.blob.declarations, comparison.parse.declarations);
    row("diagnostics", comparison.blob.diagnostics, comparison.parse.diagnostics);
    row("keyframes", comparison.blob.keyframes, comparison.parse.keyframes);
    row("layers", comparison.blob.layers, comparison.parse.layers);
    row("container queries", comparison.blob.container_queries, comparison.parse.container_queries);
    std::printf("stylesheet_benchmark: compiled fingerprint 0x%016llx\n",
                static_cast<unsigned long long>(comparison.parse.fingerprint));

    const std::size_t prepare_saved =
        comparison.parse.prepare.allocations - comparison.blob.prepare.allocations;
    const std::size_t bytes_saved =
        comparison.parse.prepare.allocations_bytes - comparison.blob.prepare.allocations_bytes;
    std::printf("stylesheet_benchmark: the blob path skips %zu tokens and saves %zu of %zu prepare "
                "allocations (%zu bytes) per startup\n",
                comparison.css_tokens, prepare_saved, comparison.parse.prepare.allocations,
                bytes_saved);
}

namespace {

bool exact(const char* name, std::size_t actual, std::size_t expected) {
    if (actual == expected) {
        return true;
    }
    std::fprintf(stderr,
                 "stylesheet_benchmark: %s is %zu, the baseline says %zu; regenerate with --update "
                 "if the change is intended\n",
                 name, actual, expected);
    return false;
}

bool tolerant(const char* name, std::size_t actual, std::size_t expected, double percent) {
    const double allowed = static_cast<double>(expected) * percent / 100.0;
    const double delta = std::abs(static_cast<double>(actual) - static_cast<double>(expected));
    if (delta <= allowed) {
        return true;
    }
    std::fprintf(stderr,
                 "stylesheet_benchmark: %s is %zu, outside the baseline %zu by more than %.1f%%; "
                 "regenerate with --update if the change is intended\n",
                 name, actual, expected, percent);
    return false;
}

} // namespace

bool compare(const PathComparison& comparison, const Baseline& baseline) {
    const double percent = baseline.allocation_tolerance_percent;
    bool ok = true;
    // Structural counters: a pure function of the stylesheet and the engine, so they are exact.
    ok = exact("stylesheet bytes", comparison.css_bytes, baseline.css_bytes) && ok;
    ok = exact("stylesheet tokens", comparison.css_tokens, baseline.css_tokens) && ok;
    ok = exact("blob bytes", comparison.blob_bytes, baseline.blob_bytes) && ok;
    ok = exact("rules", comparison.parse.rules, baseline.rules) && ok;
    ok = exact("selectors", comparison.parse.selectors, baseline.selectors) && ok;
    ok = exact("declarations", comparison.parse.declarations, baseline.declarations) && ok;
    ok = exact("diagnostics", comparison.parse.diagnostics, baseline.diagnostics) && ok;
    ok = exact("keyframes", comparison.parse.keyframes, baseline.keyframes) && ok;
    ok = exact("layers", comparison.parse.layers, baseline.layers) && ok;
    ok = exact("container queries", comparison.parse.container_queries,
               baseline.container_queries) &&
         ok;
    // Allocation counters: the standard library's container layout differs between toolchains.
    ok = tolerant("blob load allocations", comparison.blob.prepare.allocations,
                  baseline.blob_load_allocations, percent) &&
         ok;
    ok = tolerant("blob load allocation bytes", comparison.blob.prepare.allocations_bytes,
                  baseline.blob_load_allocated_bytes, percent) &&
         ok;
    ok = tolerant("parse prepare allocations", comparison.parse.prepare.allocations,
                  baseline.parse_prepare_allocations, percent) &&
         ok;
    ok = tolerant("parse prepare allocation bytes", comparison.parse.prepare.allocations_bytes,
                  baseline.parse_prepare_allocated_bytes, percent) &&
         ok;
    // The invariant the benchmark exists to protect.
    if (comparison.blob.prepare.allocations >= comparison.parse.prepare.allocations) {
        std::fprintf(stderr,
                     "stylesheet_benchmark: the blob path allocates %zu times, the parse path %zu: "
                     "the blob path no longer prepares the sheet for free\n",
                     comparison.blob.prepare.allocations, comparison.parse.prepare.allocations);
        ok = false;
    }
    return ok;
}

} // namespace tessera::benchmark
