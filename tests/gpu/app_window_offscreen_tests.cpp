// SPDX-License-Identifier: MIT
//
// The window driver's half of the offscreen layer pool (docs/design/gpu/offscreen-layers.md section
// 5e), on a real window and swapchain: six masked boxes present with both pooled targets recycled
// and nothing refused, and the same scene under a pool capped at one target paints its masks
// without the effect - which must show as a counter, as the run's stats, and as ONE logged warning,
// not as a silent difference. Skips when the platform has no window backend or presentation
// surface.

#include "tessera/app/window.hpp"
#include "tessera/diagnostics/log.hpp"
#include "tessera/diagnostics/metrics.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#ifndef TESSERA_TEST_FONT
#error "TESSERA_TEST_FONT must point at the repository test font"
#endif

namespace {

namespace diag = tessera::diagnostics;
using tessera::ui::Node;
using tessera::ui::style::compile_stylesheet;

constexpr int kBoxes = 6;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_window_offscreen: " << message << "\n";
    }
    return condition;
}

struct Outcome {
    bool available = false;
    tessera::app::WindowFrameStats stats;
};

Outcome run(std::uint32_t max_targets, diag::Logger& logger, diag::MetricsRegistry& metrics) {
    Node root(1);
    for (int index = 0; index < kBoxes; ++index) {
        Node& box = root.add_child(std::make_unique<Node>(static_cast<std::uint64_t>(index + 2)));
        box.set_element_kind("Box");
    }
    tessera::app::WindowOptions options;
    options.title = "Tessera app-window offscreen";
    options.width = 320;
    options.height = 400;
    options.visible = false;
    options.font_path = TESSERA_TEST_FONT;
    options.runtime.glass_max_blur_targets = max_targets;
    options.logger = &logger;
    options.metrics = &metrics;

    tessera::app::WindowHost host(
        root,
        compile_stylesheet("Box { width: 200px; height: 40px; background-color: #ff0000; "
                           "mask-image: linear-gradient(90deg, #000000, rgba(0, 0, 0, 0)); }"),
        options);
    Outcome outcome;
    outcome.available = host.available();
    if (outcome.available) {
        outcome.stats = host.run(4);
    }
    return outcome;
}

} // namespace

int main() {
    {
        diag::Logger logger;
        diag::MemoryLogSink sink;
        logger.add_sink(sink);
        diag::MetricsRegistry metrics;
        const Outcome roomy = run(4, logger, metrics);
        if (!roomy.available) {
            std::cout << "app_window_offscreen: skipped (no window backend or presentation "
                         "surface)\n";
            return 0;
        }
        if (!expect(roomy.stats.presented >= 3, "the masked scene presents") ||
            !expect(roomy.stats.offscreen_peak_live == 2,
                    "six masked boxes need two live targets, not twelve") ||
            !expect(roomy.stats.offscreen_refused == 0 && sink.records().empty(),
                    "nothing was refused, nothing was logged") ||
            !expect(metrics.value(0) == 0 && metrics.name(0) == "gpu.offscreen.refused",
                    "the refusal counter exists and reads zero")) {
            return 1;
        }
    }
    {
        diag::Logger logger;
        diag::MemoryLogSink sink;
        logger.add_sink(sink);
        diag::MetricsRegistry metrics;
        const Outcome starved = run(1, logger, metrics);
        if (!expect(starved.available && starved.stats.presented >= 3,
                    "the starved scene still presents") ||
            !expect(starved.stats.offscreen_refused >= static_cast<std::uint64_t>(kBoxes),
                    "every mask the one-target pool could not take is counted") ||
            !expect(metrics.value(0) == starved.stats.offscreen_refused, "and so is the metric") ||
            !expect(sink.records().size() == 1, "exactly one warning for the whole run") ||
            !expect(sink.records().front().message.find("TESSERA_GPU_OFFSCREEN_EXHAUSTED") == 0 &&
                        sink.records().front().category == diag::LogCategory::Gpu,
                    "carrying the code, in the gpu category")) {
            return 2;
        }
    }
    std::cout << "app_window_offscreen: ok\n";
    return 0;
}
