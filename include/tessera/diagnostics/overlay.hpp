// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/diagnostics/metrics.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tessera::diagnostics {

// Plain snapshot of the runtime frame state, decoupled from runtime headers so the overlay can be
// tested and reused without linking tessera_runtime.
struct OverlayFrame {
    std::uint64_t frame = 0;
    std::uint32_t steps = 0;
    std::size_t completions = 0;
    std::size_t pending = 0;
    std::size_t damage_rects = 0;
    std::size_t scissors = 0;
    bool full_redraw = false;
    // Frame budget (issue #183, docs/specs/chart-node-v1.md section 7): the duration of the frame
    // just completed and the budget it is judged against. `budget_ms` of 0 means no budget is
    // configured, and the overlay then omits the budget line entirely rather than printing a
    // meaningless ratio.
    double frame_ms = 0.0;
    double budget_ms = 0.0;
    std::uint64_t budget_overruns = 0;
};

// Builds short overlay lines from a frame snapshot and metric values. No allocation happens per
// call beyond the returned vector; the renderer owns the font/text pipeline.
class OverlayModel {
public:
    [[nodiscard]] static std::vector<std::string> build(const OverlayFrame& frame,
                                                        std::span<const MetricSample> metrics,
                                                        std::size_t max_metrics = 4);
};

} // namespace tessera::diagnostics
