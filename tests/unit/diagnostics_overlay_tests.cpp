// SPDX-License-Identifier: MIT

#include "tessera/diagnostics/overlay.hpp"

#include <iostream>
#include <string>

namespace {

using tessera::diagnostics::MetricKind;
using tessera::diagnostics::MetricSample;
using tessera::diagnostics::OverlayFrame;
using tessera::diagnostics::OverlayModel;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "diagnostics_overlay: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    OverlayFrame frame;
    frame.frame = 42;
    frame.steps = 2;
    frame.completions = 3;
    frame.pending = 1;
    frame.damage_rects = 5;
    frame.scissors = 2;
    frame.full_redraw = false;

    const MetricSample metrics[] = {
        {"paint.draw_calls", 12, MetricKind::Counter},
        {"async.queue_depth", 4, MetricKind::Gauge},
    };

    const auto lines = OverlayModel::build(frame, metrics, 1);
    if (!expect(lines.size() == 3, "two summary lines plus one metric") ||
        !expect(lines[0] == "frame 42  steps 2  completions 3/1", "frame line") ||
        !expect(lines[1] == "damage 5  scissors 2  full no", "damage line") ||
        !expect(lines[2] == "paint.draw_calls 12", "metric line")) {
        return 1;
    }

    frame.full_redraw = true;
    const auto full = OverlayModel::build(frame, metrics, 4);
    if (!expect(full.size() == 4, "all metrics") ||
        !expect(full[1] == "damage 5  scissors 2  full yes", "full redraw yes") ||
        !expect(full[3] == "async.queue_depth 4", "second metric")) {
        return 2;
    }

    const auto empty = OverlayModel::build(frame, {}, 4);
    if (!expect(empty.size() == 2, "summary without metrics")) {
        return 3;
    }

    std::cout << "diagnostics_overlay: ok\n";
    return 0;
}
