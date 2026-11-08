// SPDX-License-Identifier: MIT

#include "tessera/diagnostics/metrics.hpp"

#include <iostream>

namespace {

using tessera::diagnostics::MetricKind;
using tessera::diagnostics::MetricsRegistry;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "diagnostics_metrics: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    MetricsRegistry metrics(4);
    if (!expect(metrics.size() == 0, "initially empty") ||
        !expect(metrics.snapshot().empty(), "empty snapshot") ||
        !expect(metrics.add_counter("") == MetricsRegistry::kInvalidMetric,
                "empty name rejected")) {
        return 1;
    }

    const std::size_t draws = metrics.add_counter("paint.draw_calls");
    const std::size_t depth = metrics.add_gauge("async.queue_depth");
    if (!expect(draws == 0 && depth == 1, "registration order") ||
        !expect(metrics.kind(draws) == MetricKind::Counter, "counter kind") ||
        !expect(metrics.kind(depth) == MetricKind::Gauge, "gauge kind") ||
        !expect(metrics.name(draws) == "paint.draw_calls", "counter name") ||
        !expect(metrics.size() == 2, "two metrics")) {
        return 2;
    }

    metrics.increment(draws);
    metrics.increment(draws, 3);
    metrics.set(depth, 7);
    if (!expect(metrics.value(draws) == 4, "counter accumulates") ||
        !expect(metrics.value(depth) == 7, "gauge set")) {
        return 3;
    }

    metrics.increment(depth);
    if (!expect(metrics.value(depth) == 8, "gauge can increment")) {
        return 4;
    }

    const auto samples = metrics.snapshot();
    if (!expect(samples.size() == 2, "snapshot size") ||
        !expect(samples[0].name == "paint.draw_calls" && samples[0].value == 4,
                "snapshot counter") ||
        !expect(samples[1].name == "async.queue_depth" && samples[1].value == 8,
                "snapshot gauge") ||
        !expect(samples[1].kind == MetricKind::Gauge, "snapshot kind")) {
        return 5;
    }

    const std::size_t frame = metrics.add_counter("runtime.frame_time_us");
    const std::size_t damage = metrics.add_counter("paint.damage_rects");
    metrics.set(frame, 16);
    metrics.set(damage, 3);
    if (!expect(metrics.size() == 4, "capacity reached") ||
        !expect(metrics.add_counter("overflow") == MetricsRegistry::kInvalidMetric,
                "capacity rejects new metrics") ||
        !expect(metrics.value(MetricsRegistry::kInvalidMetric) == 0, "invalid id reads zero")) {
        return 6;
    }

    metrics.increment(MetricsRegistry::kInvalidMetric);
    metrics.set(MetricsRegistry::kInvalidMetric, 9);

    metrics.reset();
    if (!expect(metrics.value(draws) == 0 && metrics.value(depth) == 0, "reset zeroes values") ||
        !expect(metrics.size() == 4, "reset keeps registrations") ||
        !expect(metrics.snapshot()[0].value == 0, "snapshot after reset")) {
        return 7;
    }

    std::cout << "diagnostics_metrics: ok\n";
    return 0;
}
