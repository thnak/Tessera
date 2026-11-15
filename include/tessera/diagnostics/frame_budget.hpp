// SPDX-License-Identifier: MIT
//
// Frame-budget metrics (issue #183): the numbers the diagnostics overlay reports so a frame can be
// judged against its budget instead of against a stopwatch. Normative contract:
// docs/specs/chart-node-v1.md section 7 and docs/design/performance.md section 6.
//
// The budget itself is not computed here: AdaptiveQualityController::budget_ms() already owns the
// derivation (`frameBudgetMs`, else 1000 / target_fps) and the ladder that acts on a miss. This
// type is the reporting seam beside it - it registers three stable metric names on the diagnostics
// registry, records one sample per frame, and fills the overlay fields. Registration allocates the
// slot names once; record() is three atomic stores and a compare, so it is safe on the frame path.
//
// Units: the registry stores integers, so the two durations are published in microseconds under
// `frame.time_us` and `frame.budget_us`. The overlay prints milliseconds, which is the unit the
// budget is configured in.

#pragma once

#include "tessera/diagnostics/metrics.hpp"
#include "tessera/diagnostics/overlay.hpp"

#include <cstdint>

namespace tessera::diagnostics {

// The three values one reported frame carries, and the budget it is judged against.
struct FrameBudgetSample {
    double frame_ms = 0.0;
    double budget_ms = 0.0;
    std::uint64_t overruns = 0;

    [[nodiscard]] bool over_budget() const noexcept {
        return budget_ms > 0.0 && frame_ms > budget_ms;
    }
    [[nodiscard]] friend bool operator==(const FrameBudgetSample&,
                                         const FrameBudgetSample&) = default;
};

// Publishes frame time against the frame budget.
//
// `budget_ms` of 0 means "no budget" (the runtime-config rule: a 0 `frameBudgetMs` with no target
// rate leaves the budget unset), in which case no frame is ever counted as an overrun and the
// overlay omits the line entirely. Rebinding the budget later is allowed: `set_budget` updates the
// gauge and the threshold, and the overrun counter keeps its history.
class FrameBudgetRecorder {
public:
    // Registers `frame.time_us`, `frame.budget_us` (gauges) and `frame.budget_overruns` (counter).
    // `registry` is borrowed and must outlive this object.
    FrameBudgetRecorder(MetricsRegistry& registry, double budget_ms)
        : registry_(&registry) {
        time_id_ = registry.add_gauge("frame.time_us");
        budget_id_ = registry.add_gauge("frame.budget_us");
        overruns_id_ = registry.add_counter("frame.budget_overruns");
        set_budget(budget_ms);
    }

    FrameBudgetRecorder(const FrameBudgetRecorder&) = delete;
    FrameBudgetRecorder& operator=(const FrameBudgetRecorder&) = delete;

    // Applies a new budget. Negative and non-finite values are clamped to 0 (no budget), matching
    // AdaptiveQualityConfig's validation instead of inventing a second rule.
    void set_budget(double budget_ms) noexcept {
        budget_ms_ = (budget_ms > 0.0 && budget_ms == budget_ms) ? budget_ms : 0.0;
        registry_->set(budget_id_, to_microseconds(budget_ms_));
    }

    // One frame. Stores the duration, and counts an overrun when the budget is set and exceeded.
    // A frame that exactly equals the budget is inside it.
    void record(double frame_ms) noexcept {
        const double duration = (frame_ms > 0.0) ? frame_ms : 0.0;
        frame_ms_ = duration;
        registry_->set(time_id_, to_microseconds(duration));
        if (budget_ms_ > 0.0 && duration > budget_ms_) {
            ++overruns_;
            registry_->increment(overruns_id_);
        }
    }

    [[nodiscard]] FrameBudgetSample sample() const noexcept {
        return FrameBudgetSample{frame_ms_, budget_ms_, overruns_};
    }
    [[nodiscard]] double budget_ms() const noexcept {
        return budget_ms_;
    }

    // Fills the overlay's budget fields. Called by whatever builds the overlay frame; the overlay
    // itself stays a plain snapshot type.
    void apply(OverlayFrame& frame) const noexcept {
        frame.frame_ms = frame_ms_;
        frame.budget_ms = budget_ms_;
        frame.budget_overruns = overruns_;
    }

    [[nodiscard]] std::size_t time_metric() const noexcept {
        return time_id_;
    }
    [[nodiscard]] std::size_t budget_metric() const noexcept {
        return budget_id_;
    }
    [[nodiscard]] std::size_t overruns_metric() const noexcept {
        return overruns_id_;
    }

private:
    // Rounds to the nearest microsecond; a sub-microsecond frame reports 0 rather than lying
    // upward.
    [[nodiscard]] static std::uint64_t to_microseconds(double milliseconds) noexcept {
        const double microseconds = milliseconds * 1000.0;
        if (!(microseconds > 0.0)) {
            return 0;
        }
        return static_cast<std::uint64_t>(microseconds + 0.5);
    }

    MetricsRegistry* registry_ = nullptr;
    std::size_t time_id_ = MetricsRegistry::kInvalidMetric;
    std::size_t budget_id_ = MetricsRegistry::kInvalidMetric;
    std::size_t overruns_id_ = MetricsRegistry::kInvalidMetric;
    double frame_ms_ = 0.0;
    double budget_ms_ = 0.0;
    std::uint64_t overruns_ = 0;
};

} // namespace tessera::diagnostics
