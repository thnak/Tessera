// SPDX-License-Identifier: MIT

#include "benchmark_budget.hpp"
#include "benchmark_fixture.hpp"
#include "benchmark_profile.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::benchmark {
namespace {

struct Options {
    bool json = false;
    bool timings = true;
    std::size_t runs = 9;
    BackendKind backend = BackendKind::Null;
};

[[nodiscard]] bool parse(int argc, char** argv, Options& options) {
    for (int index = 2; index < argc; ++index) {
        const std::string_view arg(argv[index]);
        if (arg == "--json") {
            options.json = true;
        } else if (arg == "--no-timings") {
            options.timings = false;
        } else if (arg == "--runs" && index + 1 < argc) {
            options.runs = static_cast<std::size_t>(std::atoi(argv[++index]));
        } else if (arg == "--backend" && index + 1 < argc) {
            const std::string_view value(argv[++index]);
            if (value == "vulkan") {
                options.backend = BackendKind::Vulkan;
            } else if (value != "null") {
                return false;
            }
        } else {
            return false;
        }
    }
    return options.runs > 0;
}

// Runs `runs` fresh fixtures (plus one discarded warm-up) and folds them into a report.
[[nodiscard]] bool collect(const Options& options, ProfileReport& report, std::string& error) {
    FixtureOptions fixture_options;
    fixture_options.backend = options.backend;
    fixture_options.storage_pipeline = true;
    std::vector<std::vector<SceneProfile>> runs;
    for (std::size_t run = 0; run < options.runs + (options.timings ? 1 : 0); ++run) {
        Fixture fixture(fixture_options);
        if (fixture.nodes() != kBenchmarkNodes) {
            error = "the scene built an unexpected node count";
            return false;
        }
        std::vector<SceneProfile> scenes = fixture.profile();
        // The same cold frame with paint culling off: every one of the 12 000 nodes is painted, the
        // case a screen with its whole tree visible would hit.
        FixtureOptions unculled = fixture_options;
        unculled.cull_paint = false;
        Fixture whole_tree(unculled);
        scenes.push_back(whole_tree.profile_cold_frame("cold_unculled"));
        if (run > 0 || !options.timings) {
            runs.push_back(std::move(scenes));
        }
    }
    report.backend = options.backend == BackendKind::Vulkan ? "vulkan" : "null";
    report.nodes = kBenchmarkNodes;
    if (!aggregate_runs(runs, report, error)) {
        return false;
    }
    if (options.timings) {
        std::vector<double> compile;
        for (std::size_t run = 0; run < options.runs; ++run) {
            const auto begin = std::chrono::steady_clock::now();
            const auto sheet = compile_benchmark_stylesheet();
            const auto end = std::chrono::steady_clock::now();
            (void)sheet;
            compile.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
        }
        report.stylesheet_compile_ms = summarize(std::move(compile)).median;
    }
    return true;
}

} // namespace

int run_stage_profile(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        std::fprintf(stderr,
                     "usage: tessera_benchmark --stage-profile [--json] [--no-timings] [--runs N] "
                     "[--backend null|vulkan]\n");
        return 2;
    }
    ProfileReport report;
    std::string error;
    try {
        if (!collect(options, report, error)) {
            std::fprintf(stderr, "stage_profile: %s\n", error.c_str());
            return 1;
        }
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "stage_profile: %s\n", failure.what());
        return 3;
    }
    const std::string text = options.json ? format_profile_json(report, options.timings)
                                          : format_profile_text(report, options.timings);
    std::fputs(text.c_str(), stdout);
    return 0;
}

int run_profile_check() {
    Options options;
    options.timings = false;
    options.runs = 2;
    ProfileReport first;
    std::string error;
    if (!collect(options, first, error)) {
        std::fprintf(stderr, "stage_profile_check: %s\n", error.c_str());
        return 1;
    }
    ProfileReport second;
    if (!collect(options, second, error)) {
        std::fprintf(stderr, "stage_profile_check: %s\n", error.c_str());
        return 1;
    }
    bool ok = true;
    const auto expect = [&](bool condition, const char* what) {
        if (!condition) {
            std::fprintf(stderr, "stage_profile_check: failed: %s\n", what);
            ok = false;
        }
    };
    // Determinism: two independent collections print the same bytes, text and JSON.
    expect(format_profile_text(first, false) == format_profile_text(second, false),
           "the counter report is byte-identical between collections");
    expect(format_profile_json(first, false) == format_profile_json(second, false),
           "the JSON report is byte-identical between collections");
    expect(format_profile_text(first, false).find("stage (ms)") == std::string::npos,
           "the deterministic form carries no timing table");
    expect(format_profile_json(first, false).find("stage_ms") == std::string::npos,
           "the deterministic JSON carries no timing field");

    const auto find = [&](const char* name) -> const SceneProfile* {
        for (const SceneProfile& scene : first.scenes) {
            if (scene.name == name) {
                return &scene;
            }
        }
        return nullptr;
    };
    const SceneProfile* cold = find("cold_first_frame");
    const SceneProfile* damaged = find("text_edit_damaged");
    const SceneProfile* repeat = find("repeat_full_repaint");
    const SceneProfile* glass = find("glass_full_paint");
    const SceneProfile* unculled = find("cold_unculled");
    expect(first.scenes.size() == 7 && cold && damaged && repeat && glass && unculled,
           "seven named scenes");
    if (ok) {
        expect(unculled->counters.paint_nodes > cold->counters.paint_nodes,
               "with paint culling off the pass visits more nodes");
        expect(cold->counters.draw_calls > 0 && cold->counters.quads > 0, "the cold frame draws");
        expect(cold->counters.vertex_bytes == cold->counters.quads * 6u * 64u,
               "vertex bytes are quads x 6 vertices x 64 bytes");
        expect(cold->counters.clip_upload_bytes > 0 && cold->counters.storage_buffer_binds > 0,
               "the clip buffer is uploaded and bound");
        expect(cold->counters.render_passes == 1, "a flat frame is one render pass");
        expect(damaged->counters.full_redraw == 0 && damaged->counters.damage_rects > 0,
               "the damaged frame is partial");
        expect(damaged->counters.paint_nodes < cold->counters.paint_nodes,
               "a damaged paint visits fewer nodes than a full one");
        expect(repeat->counters.paint_nodes <= cold->counters.paint_nodes,
               "a retained replay never visits more nodes than the cold paint");
        expect(glass->counters.glass_draws == 1 && glass->counters.region_copies > 0 &&
                   glass->counters.render_passes > 1,
               "the glass frame breaks the pass for its backdrop");
    }
    if (!ok) {
        return 1;
    }
    std::printf("stage_profile_check: ok\n");
    return 0;
}

} // namespace tessera::benchmark
