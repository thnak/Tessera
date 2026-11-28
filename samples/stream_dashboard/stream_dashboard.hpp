// SPDX-License-Identifier: MIT
//
// The streaming dashboard sample (issue #184, V1-M6 acceptance). Four chart panels fed by a
// scripted, fully deterministic load at different rates, a work meter, and the shared damage ->
// redraw-plan -> ClippedPaintSink path, so a frame repaints only what changed. Nothing here reads a
// clock, a random device or the system: the load is an integer function of the frame index, so the
// same frame index is the same picture on every machine (the committed goldens rely on that).
//
// The sample owns no renderer. paint() writes into any ui::paint::PaintSink: the goldens use the
// CPU sink, a window driver would use the batched 2D sink. Steady-state step() and paint() allocate
// nothing (tests/gpu/stream_dashboard_allocation.cpp); only the redraw plan, which is the shared
// runtime's value type, allocates its scissor list.

#pragma once

#include "tessera/rendering/damage.hpp"
#include "tessera/rendering/redraw_plan.hpp"
#include "tessera/ui/chart/chart_node.hpp"
#include "tessera/ui/paint/paint_sink.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace tessera::samples::stream {

inline constexpr std::uint32_t kWidth = 640;
inline constexpr std::uint32_t kHeight = 400;
inline constexpr std::size_t kChartCount = 4;
inline constexpr std::size_t kSeriesPerChart = 2;
inline constexpr std::size_t kCapacity = 160;

// What one frame did. Counts, not times, so tests can assert on them deterministically.
struct FrameWork {
    std::size_t charts_updated = 0;
    std::size_t samples_appended = 0;
    // Segments retained by the charts that were rebuilt this frame: the work a full rebuild costs.
    std::size_t segments_rebuilt = 0;
    std::size_t damage_rects = 0;
    bool full_redraw = false;
};

class StreamDashboard {
public:
    StreamDashboard();

    // One frame of the scripted load: appends the scheduled samples, rebuilds the touched charts
    // and feeds their damage (plus the meter's) to the shared accumulator. Allocation-free.
    void step();

    // Turns the accumulated damage into this frame's plan. The first frame, and any frame the
    // shared policy rejects as too large, is a full redraw.
    [[nodiscard]] rendering::RedrawPlan make_plan() const;

    // Paints the whole scene.
    void paint(ui::paint::PaintSink& sink) const;

    [[nodiscard]] std::uint64_t frame() const noexcept {
        return frame_;
    }
    [[nodiscard]] const FrameWork& work() const noexcept {
        return work_;
    }
    [[nodiscard]] const rendering::DamageAccumulator& damage() const noexcept {
        return damage_;
    }
    [[nodiscard]] const ui::chart::ChartNode& chart(std::size_t index) const noexcept {
        return charts_[index];
    }
    // The most boxes one full paint can emit: bounded by the ring capacity, never by run length.
    [[nodiscard]] static constexpr std::size_t max_boxes() noexcept {
        return kChartCount * kSeriesPerChart * (kCapacity - 1) + 64;
    }

private:
    [[nodiscard]] rendering::rhi::Rect meter_fill() const noexcept;

    std::array<ui::chart::ChartNode, kChartCount> charts_;
    std::array<std::array<std::uint32_t, kSeriesPerChart>, kChartCount> rng_{};
    std::array<std::uint64_t, kChartCount> counters_{};
    rendering::DamageAccumulator damage_;
    FrameWork work_;
    rendering::rhi::Rect meter_{};
    std::uint64_t frame_ = 0;
};

} // namespace tessera::samples::stream
