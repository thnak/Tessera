// SPDX-License-Identifier: MIT

#include "stream_dashboard.hpp"
#include "tessera/ui/chart/chart_render.hpp"

#include <algorithm>
#include <utility>

namespace tessera::samples::stream {
namespace {

namespace rhi = tessera::rendering::rhi;
using ui::chart::ChartNode;
using ui::chart::ChartSample;
using ui::chart::ChartSeriesStyle;

// The load script. Each chart updates on its own period and appends `samples` per update, so a
// frame touches one to four charts: busy frames force a full redraw, quiet ones a partial one.
struct Schedule {
    std::uint32_t period;
    std::uint32_t samples;
    std::uint32_t wave_step; // triangle-wave advance per sample
    std::uint32_t phase;
};
constexpr std::array<Schedule, kChartCount> kSchedule{{
    {1, 3, 3, 0},
    {2, 2, 5, 40},
    {3, 2, 7, 90},
    {5, 1, 11, 150},
}};

constexpr rhi::Rect kCards[kChartCount] = {
    {16.0f, 48.0f, 300.0f, 164.0f},
    {324.0f, 48.0f, 300.0f, 164.0f},
    {16.0f, 220.0f, 300.0f, 164.0f},
    {324.0f, 220.0f, 300.0f, 164.0f},
};
constexpr float kPad = 12.0f;
constexpr rhi::Rect kMeterTrack{404.0f, 10.0f, 220.0f, 12.0f};

constexpr rhi::Color kBackground = rhi::rgba8(11, 17, 32);
constexpr rhi::Color kHeader = rhi::rgba8(17, 24, 39);
constexpr rhi::Color kCard = rhi::rgba8(30, 41, 59);
constexpr rhi::Color kGrid = rhi::rgba8(51, 65, 85);
constexpr rhi::Color kTrack = rhi::rgba8(15, 23, 42);
constexpr rhi::Color kMeterOk = rhi::rgba8(34, 197, 94);
constexpr rhi::Color kMeterBusy = rhi::rgba8(250, 204, 21);
constexpr rhi::Color kSeriesColors[kChartCount][kSeriesPerChart] = {
    {rhi::rgba8(56, 189, 248), rhi::rgba8(244, 114, 182)},
    {rhi::rgba8(163, 230, 53), rhi::rgba8(251, 146, 60)},
    {rhi::rgba8(167, 139, 250), rhi::rgba8(45, 212, 191)},
    {rhi::rgba8(250, 204, 21), rhi::rgba8(248, 113, 113)},
};

rhi::Rect plot_for(std::size_t chart) {
    const rhi::Rect& card = kCards[chart];
    return {card.x + kPad, card.y + kPad, card.width - 2.0f * kPad, card.height - 2.0f * kPad};
}

// xorshift32: an integer generator, so the same seed is the same stream on every platform.
std::uint32_t next_random(std::uint32_t& state) noexcept {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// A triangle wave in [15, 85] plus integer noise in [-5, 5], offset per series; all integer until
// the final conversion, which is exact for these magnitudes.
float load_value(const Schedule& schedule, std::size_t series, std::uint64_t index,
                 std::uint32_t& rng) noexcept {
    const std::uint32_t position = static_cast<std::uint32_t>(
        (index * schedule.wave_step + schedule.phase + series * 60U) % 200U);
    const std::uint32_t tri = position < 100U ? position : 200U - position;
    const std::int32_t noise = static_cast<std::int32_t>(next_random(rng) % 11U) - 5;
    const std::int32_t value = 15 + static_cast<std::int32_t>(tri * 7U / 10U) + noise;
    return static_cast<float>(std::clamp(value, 0, 100));
}

ChartNode make_chart(std::size_t index) {
    ChartNode chart(kSeriesPerChart, kCapacity, plot_for(index));
    for (std::size_t series = 0; series < kSeriesPerChart; ++series) {
        chart.set_style(series, ChartSeriesStyle{kSeriesColors[index][series], 2.0f, 0.0f});
    }
    chart.set_value_range(ui::chart::ChartValueRange{0.0f, 100.0f});
    return chart;
}

} // namespace

StreamDashboard::StreamDashboard()
    : charts_{make_chart(0), make_chart(1), make_chart(2), make_chart(3)},
      damage_(rendering::DamageConfig{{kWidth, kHeight}, 16}),
      meter_(meter_fill()) {
    for (std::size_t chart = 0; chart < kChartCount; ++chart) {
        for (std::size_t series = 0; series < kSeriesPerChart; ++series) {
            rng_[chart][series] = 0x9E3779B9U + static_cast<std::uint32_t>(chart * 16 + series);
        }
    }
}

rhi::Rect StreamDashboard::meter_fill() const noexcept {
    // The meter shows how many chart segments the last frame rebuilt, as a share of every segment
    // the four charts can hold.
    constexpr std::size_t kTotal = kChartCount * kSeriesPerChart * (kCapacity - 1);
    const std::size_t clamped = std::min(work_.segments_rebuilt, kTotal);
    const float width = static_cast<float>(clamped * 220U / kTotal);
    return {kMeterTrack.x, kMeterTrack.y, width, kMeterTrack.height};
}

void StreamDashboard::step() {
    damage_.clear();
    work_ = FrameWork{};

    for (std::size_t chart = 0; chart < kChartCount; ++chart) {
        const Schedule& schedule = kSchedule[chart];
        if (frame_ % schedule.period != 0) {
            continue;
        }
        for (std::uint32_t n = 0; n < schedule.samples; ++n) {
            const std::uint64_t index = counters_[chart]++;
            for (std::size_t series = 0; series < kSeriesPerChart; ++series) {
                const float value = load_value(schedule, series, index, rng_[chart][series]);
                if (charts_[chart].append(series, ChartSample{static_cast<double>(index), value})) {
                    ++work_.samples_appended;
                }
            }
        }
        const ui::chart::ChartDamage& damage = charts_[chart].rebuild();
        for (const rhi::Rect& rect : damage) {
            damage_.add(rect);
        }
        ++work_.charts_updated;
        work_.segments_rebuilt += charts_[chart].retained_segments();
    }

    // The meter repaints when its fill width changes (its colour is a function of the width).
    const rhi::Rect previous = meter_;
    meter_ = meter_fill();
    if (previous.width != meter_.width) {
        damage_.add(kMeterTrack);
    }

    if (frame_ == 0) {
        damage_.force_full_redraw();
    }
    work_.damage_rects = damage_.rects().size();
    work_.full_redraw = damage_.full_redraw();
    ++frame_;
}

rendering::RedrawPlan StreamDashboard::make_plan() const {
    rendering::RedrawPlan plan = rendering::make_redraw_plan(damage_);
    if (rendering::should_full_redraw(plan, damage_.target())) {
        plan.full_redraw = true;
        plan.scissors.assign(1, rhi::Scissor{0, 0, kWidth, kHeight});
    }
    return plan;
}

void StreamDashboard::paint(ui::paint::PaintSink& sink) const {
    sink.fill_rect({0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight)},
                   kBackground);
    sink.fill_rect({0.0f, 0.0f, static_cast<float>(kWidth), 32.0f}, kHeader);
    sink.fill_rect(kMeterTrack, kTrack);
    // Decided from the fill width, the one thing damage tracks: a colour that could change without
    // the width changing would not be repainted by a partial redraw.
    const bool busy = meter_.width > kMeterTrack.width * 0.5f;
    if (meter_.width > 0.0f) {
        sink.fill_rect(meter_, busy ? kMeterBusy : kMeterOk);
    }
    for (std::size_t chart = 0; chart < kChartCount; ++chart) {
        sink.fill_rect(kCards[chart], kCard);
        const rhi::Rect plot = plot_for(chart);
        for (int line = 1; line < 4; ++line) {
            sink.fill_rect(
                {plot.x, plot.y + plot.height * static_cast<float>(line) * 0.25f, plot.width, 1.0f},
                kGrid);
        }
        ui::chart::paint_chart(charts_[chart], sink);
    }
}

} // namespace tessera::samples::stream
