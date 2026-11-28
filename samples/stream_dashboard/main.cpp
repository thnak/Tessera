// SPDX-License-Identifier: MIT
//
// Drives the streaming dashboard for N frames and reports frame-budget numbers (issue #184).
//
//   stream_dashboard [--frames N] [--budget-ms MS] [--out FILE.png] [--enforce]
//
// Each frame is timed on the wall clock (step + plan + paint into the CPU canvas) and recorded in a
// diagnostics::FrameBudgetRecorder against the budget (16.67 ms, 60 Hz, by default). The timings
// are machine-dependent and are reported, never compared with a golden; --enforce exits 1 when any
// frame overran, which is how a lane that cares gates on them. The picture is deterministic: --out
// writes the last frame.

#include "cpu_paint_sink.hpp"
#include "stream_dashboard.hpp"
#include "tessera/diagnostics/frame_budget.hpp"
#include "tessera/diagnostics/metrics.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/ui/paint/clipped_paint_sink.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <stb_image_write.h>

namespace {

namespace stream = tessera::samples::stream;

} // namespace

int main(int argc, char** argv) {
    std::uint64_t frames = 600;
    double budget_ms = 1000.0 / 60.0;
    std::string out;
    bool enforce = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            frames = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--budget-ms" && i + 1 < argc) {
            budget_ms = std::strtod(argv[++i], nullptr);
        } else if (arg == "--out" && i + 1 < argc) {
            out = argv[++i];
        } else if (arg == "--enforce") {
            enforce = true;
        } else {
            std::cerr << "stream_dashboard: unknown argument " << arg << "\n";
            return 2;
        }
    }

    tessera::diagnostics::MetricsRegistry registry;
    tessera::diagnostics::FrameBudgetRecorder budget(registry, budget_ms);
    stream::StreamDashboard dashboard;
    tessera::rendering::software::CpuCanvas canvas(stream::kWidth, stream::kHeight);
    tessera::poc::batched2d::CpuPaintSink sink(canvas);

    std::vector<double> times;
    times.reserve(frames);
    std::uint64_t full_frames = 0;
    for (std::uint64_t n = 0; n < frames; ++n) {
        const auto begin = std::chrono::steady_clock::now();
        dashboard.step();
        const tessera::rendering::RedrawPlan plan = dashboard.make_plan();
        if (plan.full_redraw) {
            dashboard.paint(sink);
            ++full_frames;
        } else {
            tessera::ui::paint::ClippedPaintSink clipped(sink, plan.scissors);
            dashboard.paint(clipped);
        }
        const std::chrono::duration<double, std::milli> elapsed =
            std::chrono::steady_clock::now() - begin;
        budget.record(elapsed.count());
        times.push_back(elapsed.count());
    }

    std::sort(times.begin(), times.end());
    const auto at = [&](double q) {
        return times.empty() ? 0.0 : times[static_cast<std::size_t>(q * (times.size() - 1))];
    };
    const auto sample = budget.sample();
    std::cout << "stream_dashboard: frames=" << frames << " full_redraws=" << full_frames
              << " p50=" << at(0.5) << "ms p99=" << at(0.99) << "ms max=" << at(1.0)
              << "ms budget=" << sample.budget_ms << "ms overruns=" << sample.overruns << "\n";

    if (!out.empty()) {
        const bool ok = stbi_write_png(out.c_str(), static_cast<int>(stream::kWidth),
                                       static_cast<int>(stream::kHeight), 4, canvas.pixels().data(),
                                       static_cast<int>(stream::kWidth) * 4) != 0;
        if (!ok) {
            std::cerr << "stream_dashboard: cannot write " << out << "\n";
            return 1;
        }
    }
    return enforce && sample.overruns != 0 ? 1 : 0;
}
