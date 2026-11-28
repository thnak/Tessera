// SPDX-License-Identifier: MIT
//
// The window diagnostics overlay model (issue #1024): the frame-time meter and the one function
// that folds frame, budget and layer cache into the lines the overlay prints. Pure, so no window or
// device: the draw only places these lines.

#include "tessera/diagnostics/overlay.hpp"
#include "tessera/runtime/config.hpp"
#include "window_overlay.hpp"

#include <iostream>
#include <string>

namespace {

bool expect(bool ok, const char* what) {
    if (!ok) {
        std::cerr << "window_overlay: FAIL " << what << "\n";
    }
    return ok;
}

} // namespace

int main() {
    using namespace tessera;
    using namespace tessera::app;

    RuntimeConfig defaults;
    if (!expect(!defaults.diagnostics_overlay, "the overlay is off by default")) {
        return 1;
    }

    FrameRateMeter meter;
    if (!expect(meter.fps() == 0.0, "no sample, no fps")) {
        return 2;
    }
    meter.record(0.0);
    meter.record(-3.0);
    if (!expect(meter.fps() == 0.0 && meter.overruns() == 0, "a non-positive cost is ignored")) {
        return 3;
    }
    meter.record(10.0, 8.0);
    if (!expect(meter.frame_ms() == 10.0 && meter.fps() == 100.0 && meter.overruns() == 1,
                "the first sample seeds the average and counts as an overrun")) {
        return 4;
    }
    meter.record(20.0, 8.0);
    if (!expect(meter.frame_ms() == 11.0, "the average moves a tenth of the way")) {
        return 5;
    }

    WindowOverlayInput input;
    input.frame = 7;
    input.scissors = 2;
    input.budget_ms = 8.0;
    rendering::gpu::RetainedLayerStats cache;
    cache.entries = 2;
    cache.bytes = 4096;
    cache.hits = 9;
    ui::paint::LayerPromotionStats promotion;
    promotion.promotable = 1;

    // The budget and layer lines both appear, in the order OverlayModel documents.
    input.layer_cache_enabled = true;
    const auto lines = diagnostics::OverlayModel::build(
        build_window_overlay_frame(input, meter, cache, promotion), {}, 0);
    if (!expect(lines.size() == 5, "summary, damage, fps, budget, layers") ||
        !expect(lines[0] == "frame 7  steps 1  completions 0/0", "summary line") ||
        !expect(lines[1] == "damage 2  scissors 2  full no", "damage line") ||
        !expect(lines[2] == "fps 90.9  frame 11.0ms", "fps line") ||
        !expect(lines[3] == "frame 11.0ms  budget 8.0ms  OVER  overruns 2", "budget line") ||
        !expect(lines[4] == "layers 2  4KiB  hit 9  rec 0  refused 0  evicted 0  promoted 1",
                "layer line")) {
        for (const std::string& line : lines) {
            std::cerr << "  " << line << "\n";
        }
        return 6;
    }

    // No layer cache and no budget: only the lines that mean something are present.
    input.layer_cache_enabled = false;
    input.budget_ms = 0.0;
    const auto bare = diagnostics::OverlayModel::build(
        build_window_overlay_frame(input, FrameRateMeter{}, cache, promotion), {}, 0);
    if (!expect(bare.size() == 2, "no fps, no budget and no layers leaves two lines")) {
        return 7;
    }

    std::cout << "window_overlay: ok\n";
    return 0;
}
