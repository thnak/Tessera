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

    // Issue #183: a configured budget adds exactly one line between the damage summary and the
    // metrics, and the summary lines keep their existing format.
    frame.frame_ms = 12.0;
    frame.budget_ms = 16.666;
    frame.budget_overruns = 0;
    const auto budgeted = OverlayModel::build(frame, metrics, 1);
    if (!expect(budgeted.size() == 4, "summary + budget + one metric") ||
        !expect(budgeted[0] == "frame 42  steps 2  completions 3/1", "frame line unchanged") ||
        !expect(budgeted[1] == "damage 5  scissors 2  full yes", "damage line unchanged") ||
        !expect(budgeted[2] == "frame 12.0ms  budget 16.7ms  ok  overruns 0", "budget line") ||
        !expect(budgeted[3] == "paint.draw_calls 12", "metric after the budget line")) {
        return 4;
    }
    frame.frame_ms = 21.25;
    frame.budget_overruns = 3;
    const auto over = OverlayModel::build(frame, {}, 4);
    if (!expect(over.size() == 3, "budget line only") ||
        !expect(over[2] == "frame 21.3ms  budget 16.7ms  OVER  overruns 3", "over-budget line")) {
        return 5;
    }

    // Issue #967: the layer-cache line sits after the budget line and before the metrics, and is
    // absent unless the cache is enabled.
    frame.layers_enabled = true;
    frame.layer_entries = 3;
    frame.layer_bytes = 3 * 1024 * 1024 + 512;
    frame.layer_hits = 40;
    frame.layer_records = 3;
    frame.layer_refused = 1;
    frame.layer_evicted = 2;
    frame.layers_promoted = 2;
    const auto layered = OverlayModel::build(frame, metrics, 1);
    if (!expect(layered.size() == 5, "summary + budget + layers + one metric") ||
        !expect(layered[3] == "layers 3  3072KiB  hit 40  rec 3  refused 1  evicted 2  promoted 2",
                "layer line") ||
        !expect(layered[4] == "paint.draw_calls 12", "metric after the layer line")) {
        return 6;
    }
    frame.layers_enabled = false;
    if (!expect(OverlayModel::build(frame, metrics, 1).size() == 4,
                "layer line omitted when off")) {
        return 7;
    }

    std::cout << "diagnostics_overlay: ok\n";
    return 0;
}
