// SPDX-License-Identifier: MIT

#include "tessera/diagnostics/overlay.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace tessera::diagnostics {
namespace {

// Milliseconds with one decimal: the budget is configured in `frameBudgetMs`, so a tenth of a
// millisecond is the finest unit that is still stable to print. Rounded half away from zero, so a
// frame that lands exactly on the budget prints as the budget instead of 0.1 ms above it.
std::string milliseconds(double value) {
    char buffer[32];
    const double scaled = value < 0.0 ? 0.0 : value;
    const long long tenths = static_cast<long long>(scaled * 10.0 + 0.5);
    std::snprintf(buffer, sizeof(buffer), "%lld.%lld", tenths / 10, tenths % 10);
    return std::string(buffer);
}

} // namespace

std::vector<std::string> OverlayModel::build(const OverlayFrame& frame,
                                             std::span<const MetricSample> metrics,
                                             std::size_t max_metrics) {
    std::vector<std::string> lines;
    lines.reserve(3 + std::min(max_metrics, metrics.size()));

    lines.push_back("frame " + std::to_string(frame.frame) + "  steps " +
                    std::to_string(frame.steps) + "  completions " +
                    std::to_string(frame.completions) + "/" + std::to_string(frame.pending));
    lines.push_back("damage " + std::to_string(frame.damage_rects) + "  scissors " +
                    std::to_string(frame.scissors) + "  full " +
                    (frame.full_redraw ? "yes" : "no"));
    // Omitted when no budget is configured: a ratio against 0 ms would be a fabricated number, and
    // the overlay is the diagnostic of record for a frame-budget miss.
    if (frame.budget_ms > 0.0) {
        lines.push_back("frame " + milliseconds(frame.frame_ms) + "ms  budget " +
                        milliseconds(frame.budget_ms) + "ms  " +
                        (frame.frame_ms > frame.budget_ms ? "OVER" : "ok") + "  overruns " +
                        std::to_string(frame.budget_overruns));
    }

    for (const MetricSample& metric : metrics.first(std::min(max_metrics, metrics.size()))) {
        lines.push_back(std::string(metric.name) + " " + std::to_string(metric.value));
    }
    return lines;
}

} // namespace tessera::diagnostics
