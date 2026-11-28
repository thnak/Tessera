// SPDX-License-Identifier: MIT
//
// The streaming dashboard under sustained load (issue #184, V1-M6 acceptance), Vulkan-free:
//
//  - determinism: two dashboards run through the same scripted load produce byte-identical frames;
//  - sustained load: 900 frames (the busiest chart's ring wraps more than eight times) keep the
//  work
//    per frame and the boxes per paint bounded by the ring capacity, and both partial and full
//    redraws occur, so the run is not trivially one or the other;
//  - the incremental path: repainting only the damage plan into a persistent canvas is
//  byte-identical
//    to a fresh full paint of the same frame, at every sampled frame of the run;
//  - goldens: frames 40 (rings filling), 200 (wrapped) and 800 (steady state) against committed
//    baselines in tests/golden/baseline/stream_dashboard/, bit-exact. The CPU rasterizer is the
//    reference (docs/specs/golden-tests-v1.md); regenerate with `--update-goldens`;
//  - frame-budget accounting: a modelled per-frame cost (linear in the work counters) recorded in
//    diagnostics::FrameBudgetRecorder stays inside the 60 Hz budget for the whole run, and the same
//    run against an absurdly tight budget counts overruns, so the check is not vacuous. Real
//    timings are the CLI's business (samples/stream_dashboard/main.cpp); a test never reads a
//    clock.

#include "cpu_paint_sink.hpp"
#include "stream_dashboard.hpp"
#include "tessera/diagnostics/frame_budget.hpp"
#include "tessera/diagnostics/metrics.hpp"
#include "tessera/rendering/golden/image_diff.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/ui/paint/clipped_paint_sink.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <stb_image.h>
#include <stb_image_write.h>

#ifndef TESSERA_STREAM_GOLDEN_DIR
#error "TESSERA_STREAM_GOLDEN_DIR must point at tests/golden/baseline/stream_dashboard"
#endif

namespace {

namespace stream = tessera::samples::stream;
using tessera::poc::batched2d::CpuPaintSink;
using tessera::rendering::software::CpuCanvas;
using tessera::ui::paint::ClippedPaintSink;

constexpr std::uint64_t kFrames = 900;
constexpr std::uint64_t kGoldenFrames[] = {40, 200, 800};

bool update_goldens = false;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "stream-dashboard-golden: " << message << "\n";
    }
    return condition;
}

std::vector<std::uint8_t> bytes(const CpuCanvas& canvas) {
    return {canvas.pixels().begin(), canvas.pixels().end()};
}

std::vector<std::uint8_t> full_paint(const stream::StreamDashboard& dashboard) {
    CpuCanvas canvas(stream::kWidth, stream::kHeight);
    CpuPaintSink sink(canvas);
    dashboard.paint(sink);
    return bytes(canvas);
}

// The linear cost model of one frame, in milliseconds. The constants are deliberately generous
// (a real frame is far cheaper, see the CLI) so the model only has to prove the accounting.
double modelled_frame_ms(const stream::FrameWork& work, bool full_redraw) {
    return 0.5 + 0.004 * static_cast<double>(work.segments_rebuilt) + (full_redraw ? 3.0 : 0.5);
}

bool check_golden(std::uint64_t frame, const std::vector<std::uint8_t>& pixels) {
    const std::filesystem::path baseline = std::filesystem::path(TESSERA_STREAM_GOLDEN_DIR) /
                                           ("frame_" + std::to_string(frame) + ".png");
    const std::string name = "frame " + std::to_string(frame);
    if (update_goldens) {
        std::error_code error;
        std::filesystem::create_directories(baseline.parent_path(), error);
        const int wrote =
            stbi_write_png(baseline.string().c_str(), static_cast<int>(stream::kWidth),
                           static_cast<int>(stream::kHeight), 4, pixels.data(),
                           static_cast<int>(stream::kWidth) * 4);
        std::cout << "stream-dashboard-golden: wrote " << baseline.string() << "\n";
        return expect(wrote != 0, name + ": baseline written");
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded =
        stbi_load(baseline.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (decoded == nullptr) {
        return expect(false, name + ": cannot read baseline " + baseline.string());
    }
    const std::vector<std::uint8_t> expected(
        decoded, decoded + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    stbi_image_free(decoded);
    const auto diff = tessera::rendering::golden::compare_rgba(
        {stream::kWidth, stream::kHeight}, pixels,
        {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)}, expected, 0);
    if (diff.mismatched != 0) {
        std::cerr << "stream-dashboard-golden: " << name << " mismatches " << diff.mismatched
                  << " pixels\n";
    }
    return expect(diff.mismatched == 0 && !diff.size_mismatch,
                  name + ": matches the baseline bit-exactly");
}

bool is_golden_frame(std::uint64_t frame) {
    return std::find(std::begin(kGoldenFrames), std::end(kGoldenFrames), frame) !=
           std::end(kGoldenFrames);
}

bool run() {
    stream::StreamDashboard dashboard;
    stream::StreamDashboard twin;
    CpuCanvas incremental(stream::kWidth, stream::kHeight);
    CpuPaintSink incremental_sink(incremental);

    tessera::diagnostics::MetricsRegistry registry;
    tessera::diagnostics::FrameBudgetRecorder budget(registry, 1000.0 / 60.0);
    tessera::diagnostics::MetricsRegistry tight_registry;
    tessera::diagnostics::FrameBudgetRecorder tight(tight_registry, 0.001);

    bool ok = true;
    std::size_t partial_frames = 0;
    std::size_t full_frames = 0;
    std::size_t max_segments = 0;
    std::size_t incremental_checks = 0;

    for (std::uint64_t n = 0; n < kFrames; ++n) {
        dashboard.step();
        twin.step();
        const tessera::rendering::RedrawPlan plan = dashboard.make_plan();
        if (plan.full_redraw) {
            dashboard.paint(incremental_sink);
            ++full_frames;
        } else {
            ClippedPaintSink clipped(incremental_sink, plan.scissors);
            dashboard.paint(clipped);
            ++partial_frames;
        }
        const stream::FrameWork& work = dashboard.work();
        max_segments = std::max(max_segments, work.segments_rebuilt);
        const double cost = modelled_frame_ms(work, plan.full_redraw);
        budget.record(cost);
        tight.record(cost);

        const std::uint64_t frame = dashboard.frame(); // frames completed, 1-based
        if (frame % 5 == 0 || is_golden_frame(frame)) {
            const std::vector<std::uint8_t> full = full_paint(dashboard);
            ++incremental_checks;
            ok = expect(bytes(incremental) == full,
                        "frame " + std::to_string(frame) +
                            ": the damage-only repaint equals a full paint") &&
                 ok;
            ok = expect(full == full_paint(twin), "frame " + std::to_string(frame) +
                                                      ": two dashboards render identical frames") &&
                 ok;
            if (is_golden_frame(frame)) {
                ok = check_golden(frame, full) && ok;
            }
        }
    }

    ok = expect(dashboard.frame() == kFrames, "all frames ran") && ok;
    ok = expect(partial_frames > 100 && full_frames > 10,
                "the load produces both partial and full redraws") &&
         ok;
    ok = expect(max_segments <=
                    stream::kChartCount * stream::kSeriesPerChart * (stream::kCapacity - 1),
                "rebuilt segments per frame are bounded by the ring capacity") &&
         ok;
    for (std::size_t chart = 0; chart < stream::kChartCount; ++chart) {
        ok = expect(dashboard.chart(chart).series(0).size() <= stream::kCapacity,
                    "a ring never grows past its capacity") &&
             ok;
    }
    ok = expect(dashboard.chart(0).series(0).full(), "the busiest chart's ring wrapped") && ok;
    ok = expect(incremental_checks > 150, "the equivalence check ran often enough") && ok;
    ok = expect(budget.sample().overruns == 0, "no frame overran the 60 Hz budget model") && ok;
    ok =
        expect(tight.sample().overruns == kFrames, "an absurdly tight budget counts every frame") &&
        ok;
    std::cout << "stream-dashboard-golden: frames=" << kFrames << " partial=" << partial_frames
              << " full=" << full_frames << " max_segments=" << max_segments << "\n";
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--update-goldens") {
            update_goldens = true;
        }
    }
    if (!run()) {
        std::cerr << "stream-dashboard-golden: FAILED\n";
        return 1;
    }
    std::cout << "stream-dashboard-golden: ok\n";
    return 0;
}
