// SPDX-License-Identifier: MIT
//
// stylesheet_startup_benchmark (issue #201, ADR-048).
//
// The deterministic startup benchmark for the compiled-stylesheet blob: the same stylesheet is run
// through both startup paths and the two runs are compared on stage counters, never on wall-clock
// time (docs/guides/testing.md section 7).
//
//   blob  : the bytes codegen embeds are decoded by load_stylesheet_blob() and resolved. No
//           tokenizer, no parser, no shorthand expansion, no value validation, no @supports.
//   parse : compile_stylesheet() -- tokenize, parse, prepare, resolve -- exactly what an app pays
//           when it hands the engine CSS text at run time.
//
// Determinism: the stylesheet is embedded at build time (benchmark.manifest), the blob is produced
// once before any measurement, and every reported number is a count. No clock, no random source, no
// filesystem ordering; the benchmark runs each path twice and fails unless both runs agree exactly.
//
// The committed baseline is tests/golden/perf/stylesheet_benchmark.json. Run with --update to
// regenerate it after an intended change.

#include "benchmark_baseline.hpp"
#include "benchmark_paths.hpp"
#include "benchmark_report.hpp"
#include "stylesheet_benchmark_assets.hpp"

#include <cstdio>
#include <optional>
#include <string_view>

namespace {

constexpr std::string_view kAssetName = "stylesheets/benchmark.css";

void usage(const char* program) {
    std::fprintf(stderr, "usage: %s <baseline.json>\n       %s --update <baseline.json>\n", program,
                 program);
}

} // namespace

int main(int argc, char** argv) {
    const char* baseline_path = nullptr;
    bool update = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--update") {
            update = true;
        } else if (!argument.empty() && argument.front() == '-') {
            usage(argv[0]);
            return 2;
        } else if (baseline_path == nullptr) {
            baseline_path = argv[index];
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (baseline_path == nullptr) {
        usage(argv[0]);
        return 2;
    }

    const std::expected<tessera::ui::assets::Asset, tessera::ui::assets::AssetError> asset =
        tessera::benchmark::stylesheet::bundle().find(kAssetName);
    if (!asset) {
        std::fprintf(stderr, "stylesheet_benchmark: the embedded stylesheet is missing\n");
        return 2;
    }
    const std::string_view css(reinterpret_cast<const char*>(asset->bytes.data()),
                               asset->bytes.size());

    const std::optional<tessera::benchmark::PathComparison> comparison =
        tessera::benchmark::run_paths(css);
    if (!comparison) {
        return 1;
    }
    tessera::benchmark::print_table(*comparison);

    if (!tessera::benchmark::same_cascade(*comparison)) {
        std::fprintf(stderr,
                     "stylesheet_benchmark: the two startup paths compiled the same stylesheet "
                     "differently; the counter comparison would be meaningless\n");
        return 1;
    }

    if (update) {
        if (!tessera::benchmark::write_baseline(baseline_path, *comparison)) {
            return 1;
        }
        std::printf("stylesheet_benchmark: wrote the baseline %s\n", baseline_path);
        return 0;
    }

    tessera::benchmark::Baseline baseline;
    if (!tessera::benchmark::load_baseline(baseline_path, baseline) ||
        !tessera::benchmark::compare(*comparison, baseline)) {
        return 1;
    }
    std::printf("stylesheet_benchmark: ok\n");
    return 0;
}
