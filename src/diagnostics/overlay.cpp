// SPDX-License-Identifier: MIT

#include "tessera/diagnostics/overlay.hpp"

#include <algorithm>
#include <string>

namespace tessera::diagnostics {

std::vector<std::string> OverlayModel::build(const OverlayFrame& frame,
                                             std::span<const MetricSample> metrics,
                                             std::size_t max_metrics) {
    std::vector<std::string> lines;
    lines.reserve(2 + std::min(max_metrics, metrics.size()));

    lines.push_back("frame " + std::to_string(frame.frame) + "  steps " +
                    std::to_string(frame.steps) + "  completions " +
                    std::to_string(frame.completions) + "/" + std::to_string(frame.pending));
    lines.push_back("damage " + std::to_string(frame.damage_rects) + "  scissors " +
                    std::to_string(frame.scissors) + "  full " +
                    (frame.full_redraw ? "yes" : "no"));

    for (const MetricSample& metric : metrics.first(std::min(max_metrics, metrics.size()))) {
        lines.push_back(std::string(metric.name) + " " + std::to_string(metric.value));
    }
    return lines;
}

} // namespace tessera::diagnostics
