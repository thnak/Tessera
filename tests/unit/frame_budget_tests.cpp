// SPDX-License-Identifier: MIT
//
// Frame-budget metrics and the diagnostics overlay's budget line (issue #183): the third acceptance
// criterion, "the diagnostics overlay shows frame-budget metrics".
//
// Deterministic: the durations are plain numbers handed to the recorder, so nothing reads a clock.
// What this pins:
//  - the three metric names are registered once, with the right kinds, and each frame publishes its
//    duration in microseconds (the registry stores integers);
//  - an overrun is counted only when a budget is configured and the frame exceeds it, and a frame
//    exactly on the budget is inside it;
//  - the overlay prints one budget line, with the OVER/ok verdict and the cumulative overrun count,
//    and omits the line entirely when no budget is configured (no ratio against zero);
//  - the existing frame/damage/metric lines are unchanged when a budget is present.

#include "tessera/diagnostics/frame_budget.hpp"
#include "tessera/diagnostics/metrics.hpp"
#include "tessera/diagnostics/overlay.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

using tessera::diagnostics::FrameBudgetRecorder;
using tessera::diagnostics::FrameBudgetSample;
using tessera::diagnostics::MetricKind;
using tessera::diagnostics::MetricSample;
using tessera::diagnostics::MetricsRegistry;
using tessera::diagnostics::OverlayFrame;
using tessera::diagnostics::OverlayModel;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "frame_budget: " << message << "\n";
    }
    return condition;
}

// Fetches a metric value by its registered name, so a test does not depend on slot ordering.
std::uint64_t metric_value(const MetricsRegistry& registry, std::string_view name) {
    for (std::size_t id = 0; id < registry.size(); ++id) {
        if (registry.name(id) == name) {
            return registry.value(id);
        }
    }
    return std::numeric_limits<std::uint64_t>::max();
}

bool has_metric(const MetricsRegistry& registry, std::string_view name, MetricKind kind) {
    for (std::size_t id = 0; id < registry.size(); ++id) {
        if (registry.name(id) == name) {
            return registry.kind(id) == kind;
        }
    }
    return false;
}

bool contains(const std::vector<std::string>& lines, const std::string& needle) {
    for (const std::string& line : lines) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Registration: three stable names, the right kinds, and no duplicates.
bool check_registration() {
    MetricsRegistry registry;
    FrameBudgetRecorder recorder(registry, 16.0);
    bool ok = expect(registry.size() == 3, "three metrics are registered") &&
              expect(has_metric(registry, "frame.time_us", MetricKind::Gauge),
                     "frame.time_us is a gauge") &&
              expect(has_metric(registry, "frame.budget_us", MetricKind::Gauge),
                     "frame.budget_us is a gauge") &&
              expect(has_metric(registry, "frame.budget_overruns", MetricKind::Counter),
                     "frame.budget_overruns is a counter");
    // The budget gauge is published at construction, in microseconds.
    ok = expect(metric_value(registry, "frame.budget_us") == 16000,
                "the budget is published in microseconds") &&
         ok;
    // The ids are stable and addressable.
    ok = expect(recorder.time_metric() < registry.size(), "the time id is valid") &&
         expect(recorder.budget_metric() < registry.size(), "the budget id is valid") &&
         expect(recorder.overruns_metric() < registry.size(), "the overruns id is valid") && ok;
    return ok;
}

// Recording: the duration is published, and an overrun is counted only past the budget.
bool check_recording() {
    MetricsRegistry registry;
    FrameBudgetRecorder recorder(registry, 16.0);

    recorder.record(12.5);
    bool ok = expect(metric_value(registry, "frame.time_us") == 12500,
                     "the frame duration is published in microseconds") &&
              expect(metric_value(registry, "frame.budget_overruns") == 0,
                     "a frame inside the budget is not an overrun");
    const FrameBudgetSample inside = recorder.sample();
    ok = expect(inside.frame_ms == 12.5 && inside.budget_ms == 16.0,
                "the sample reports the pair") &&
         expect(!inside.over_budget(), "a frame inside the budget is inside") && ok;

    // Exactly on the budget is inside it (documented: `>` not `>=`).
    recorder.record(16.0);
    ok = expect(metric_value(registry, "frame.budget_overruns") == 0,
                "a frame exactly on the budget is inside it") &&
         ok;

    // Past it is an overrun, counted cumulatively.
    recorder.record(21.0);
    ok = expect(metric_value(registry, "frame.budget_overruns") == 1,
                "an over-budget frame is counted") &&
         expect(recorder.sample().over_budget(), "the sample reports the overrun") && ok;
    recorder.record(40.0);
    ok = expect(metric_value(registry, "frame.budget_overruns") == 2, "overruns accumulate") &&
         expect(recorder.sample().overruns == 2, "the sample carries the total") && ok;

    // A negative or non-finite duration is clamped to zero rather than wrapping an unsigned value.
    recorder.record(-5.0);
    ok = expect(metric_value(registry, "frame.time_us") == 0, "a negative duration clamps to 0") &&
         ok;
    recorder.record(std::nan(""));
    ok = expect(metric_value(registry, "frame.time_us") == 0, "a NaN duration clamps to 0") && ok;
    return ok;
}

// No budget: nothing is ever an overrun, and the budget gauge stays 0.
bool check_no_budget() {
    MetricsRegistry registry;
    FrameBudgetRecorder recorder(registry, 0.0);
    recorder.record(1000.0);
    bool ok =
        expect(metric_value(registry, "frame.budget_overruns") == 0,
               "without a budget nothing is an overrun") &&
        expect(metric_value(registry, "frame.budget_us") == 0, "without a budget the gauge is 0") &&
        expect(!recorder.sample().over_budget(), "without a budget nothing is over") &&
        expect(recorder.budget_ms() == 0.0, "the budget reads back as 0");
    // A negative budget is treated as "no budget", matching AdaptiveQualityConfig's validation.
    FrameBudgetRecorder negative(registry, -8.0);
    negative.record(1000.0);
    ok = expect(negative.budget_ms() == 0.0, "a negative budget is clamped to 0") && ok;
    return ok;
}

// Rebinding the budget keeps the overrun history and applies the new threshold immediately.
bool check_rebind() {
    MetricsRegistry registry;
    FrameBudgetRecorder recorder(registry, 10.0);
    recorder.record(20.0);
    bool ok = expect(recorder.sample().overruns == 1, "one overrun at 10ms");
    recorder.set_budget(50.0);
    recorder.record(20.0);
    ok = expect(recorder.sample().overruns == 1, "a raised budget stops counting overruns") &&
         expect(metric_value(registry, "frame.budget_us") == 50000,
                "the gauge follows the budget") &&
         ok;
    recorder.set_budget(1.0);
    recorder.record(20.0);
    ok = expect(recorder.sample().overruns == 2, "a lowered budget resumes counting") &&
         expect(recorder.sample().budget_ms == 1.0, "the sample follows the budget") && ok;
    return ok;
}

// apply() fills the overlay fields from the recorder.
bool check_overlay_fields() {
    MetricsRegistry registry;
    FrameBudgetRecorder recorder(registry, 16.666);
    recorder.record(12.0);
    OverlayFrame frame;
    recorder.apply(frame);
    return expect(frame.frame_ms == 12.0, "apply copies the frame duration") &&
           expect(frame.budget_ms == 16.666, "apply copies the budget") &&
           expect(frame.budget_overruns == 0, "apply copies the overrun count");
}

// The overlay's budget line: value, unit, verdict and count; omitted with no budget.
bool check_overlay_line() {
    OverlayFrame frame;
    frame.frame = 7;
    frame.steps = 1;
    frame.completions = 2;
    frame.pending = 0;
    frame.damage_rects = 3;
    frame.scissors = 1;
    frame.full_redraw = false;

    // No budget: the two summary lines only, so nothing is printed against a zero budget.
    const auto plain = OverlayModel::build(frame, {}, 4);
    bool ok =
        expect(plain.size() == 2, "no budget means no budget line") &&
        expect(plain[0] == "frame 7  steps 1  completions 2/0", "the frame line is unchanged") &&
        expect(plain[1] == "damage 3  scissors 1  full no", "the damage line is unchanged");

    // A budget inside: 12.0 / 16.7, verdict ok.
    frame.frame_ms = 12.0;
    frame.budget_ms = 16.666;
    frame.budget_overruns = 0;
    const auto inside = OverlayModel::build(frame, {}, 4);
    ok = expect(inside.size() == 3, "a budget adds one line") &&
         expect(inside[2] == "frame 12.0ms  budget 16.7ms  ok  overruns 0",
                "the inside-budget line") &&
         ok;

    // A budget exceeded: verdict OVER and the cumulative count.
    frame.frame_ms = 21.25;
    frame.budget_overruns = 3;
    const auto over = OverlayModel::build(frame, {}, 4);
    ok = expect(over.size() == 3, "the over-budget line is still one line") &&
         expect(over[2] == "frame 21.3ms  budget 16.7ms  OVER  overruns 3",
                "the over-budget line") &&
         ok;

    // Metrics still follow the budget line, and max_metrics is unchanged in meaning.
    const MetricSample metrics[] = {
        {"paint.draw_calls", 12, MetricKind::Counter},
        {"frame.budget_overruns", 3, MetricKind::Counter},
    };
    const auto with_metrics = OverlayModel::build(frame, metrics, 2);
    ok = expect(with_metrics.size() == 5,
                "the budget line sits between the summary and the metrics") &&
         expect(with_metrics[3] == "paint.draw_calls 12", "metrics still follow") &&
         expect(with_metrics[4] == "frame.budget_overruns 3", "the metric is printed") && ok;

    // A budget set but a frame that rounds to the same tenth prints as a tie, not as OVER: the
    // verdict is decided on the numbers, not on the printed text.
    frame.frame_ms = 16.6661;
    frame.budget_ms = 16.666;
    frame.budget_overruns = 5;
    const auto tie = OverlayModel::build(frame, {}, 4);
    ok = expect(contains(tie, "OVER"), "a hair over the budget still reads OVER") && ok;
    return ok;
}

// The names are lowercase dotted, like every other metric in the project.
bool check_naming() {
    MetricsRegistry registry;
    const FrameBudgetRecorder recorder(registry, 16.0);
    bool ok = true;
    for (const std::size_t id :
         {recorder.time_metric(), recorder.budget_metric(), recorder.overruns_metric()}) {
        const std::string_view name = registry.name(id);
        ok = expect(name.find('.') != std::string_view::npos, "the metric name is dotted") &&
             expect(name.find(' ') == std::string_view::npos, "the metric name has no spaces") &&
             ok;
    }
    ok = expect(registry.name(recorder.time_metric()).starts_with("frame."),
                "the metrics live in the frame namespace") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = check_registration() && ok;
    ok = check_recording() && ok;
    ok = check_no_budget() && ok;
    ok = check_rebind() && ok;
    ok = check_overlay_fields() && ok;
    ok = check_overlay_line() && ok;
    ok = check_naming() && ok;
    if (!ok) {
        std::cerr << "frame_budget: FAILED\n";
        return 1;
    }
    std::cout << "frame_budget: ok\n";
    return 0;
}
