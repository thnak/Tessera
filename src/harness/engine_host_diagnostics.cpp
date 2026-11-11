// SPDX-License-Identifier: MIT
//
// Trace and audit documents for EngineAutomationHost. They are attached to the existing diagnostics
// seams (diagnostics::TraceRecorder and diagnostics::MetricsRegistry): the host builds no second
// clock, logger, or metrics store. While diagnostics are disabled this file is never entered for
// recorder/registry work; both entry points return the documented disabled answer.

#include "tessera/automation/engine_host.hpp"
#include "tessera/diagnostics/metrics.hpp"
#include "tessera/diagnostics/trace.hpp"
#include "tessera/harness/json_write.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::automation {
namespace {

namespace harness = tessera::harness;

constexpr std::string_view kAuditVersionKey = "auditVersion";

harness::ProtocolError error(harness::ProtocolErrorCode code, std::string message,
                             std::string detail = {}) {
    return harness::ProtocolError{code, std::move(message), std::move(detail), std::nullopt};
}

std::unexpected<harness::ProtocolError> unexpected(harness::ProtocolError value) {
    return std::unexpected(std::move(value));
}

std::int64_t as_json_integer(std::uint64_t value) noexcept {
    constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return static_cast<std::int64_t>(std::min(value, kMax));
}

} // namespace

std::expected<harness::TraceArtifact, harness::ProtocolError>
EngineAutomationHost::collect_trace() {
    ++stats_.traces;
    if (!diagnostics_enabled() || diagnostics().trace == nullptr) {
        // A disabled host still answers with a valid empty Chrome document, so a script that asks
        // for a trace does not fail; it just never makes the host record anything.
        return harness::TraceArtifact{"[]", 0};
    }
    record("automation.trace", diagnostics::TraceCategory::Frame);
    const diagnostics::TraceRecorder& recorder = *diagnostics().trace;
    return harness::TraceArtifact{diagnostics::to_chrome_trace_json(recorder), recorder.size()};
}

std::expected<harness::AuditArtifact, harness::ProtocolError>
EngineAutomationHost::run_audit(std::string_view profile) {
    ++stats_.audits;
    if (!diagnostics_enabled() || diagnostics().metrics == nullptr) {
        // No score can be measured without the metrics seam, and a fabricated one would make a CI
        // gate lie. Report the disabled state instead.
        return unexpected(
            error(harness::ProtocolErrorCode::Internal,
                  "audit is disabled: attach a metrics registry and enable diagnostics",
                  std::string(profile)));
    }

    record("automation.audit", diagnostics::TraceCategory::Frame);

    // Structural checks, all derived from deterministic counters (never a wall clock). Latency
    // audits are a later slice; these only claim what the host can observe today.
    struct Check {
        std::string_view id;
        bool passed;
    };
    const std::vector<Check> checks{
        {"input-queue-overflow", !pump_.total().overflowed},
        {"snapshot-node-cap", stats_.emitted_nodes <= config_.max_snapshot_nodes},
        {"layout-passed", host_.layout_runs() > 0},
    };
    std::size_t passed = 0;
    harness::JsonValue::Array encoded_checks;
    encoded_checks.reserve(checks.size());
    for (const Check& check : checks) {
        if (check.passed) {
            ++passed;
        }
        encoded_checks.push_back(
            harness::JsonValue::object({{"id", harness::JsonValue::string(std::string(check.id))},
                                        {"passed", harness::JsonValue::boolean(check.passed)}}));
    }
    const double score =
        checks.empty() ? 0.0
                       : 100.0 * static_cast<double>(passed) / static_cast<double>(checks.size());

    harness::JsonValue::Object metrics;
    metrics.emplace_back("snapshots",
                         harness::JsonValue::integer(as_json_integer(stats_.snapshots)));
    metrics.emplace_back("actions", harness::JsonValue::integer(as_json_integer(stats_.actions)));
    metrics.emplace_back("traces", harness::JsonValue::integer(as_json_integer(stats_.traces)));
    metrics.emplace_back("audits", harness::JsonValue::integer(as_json_integer(stats_.audits)));
    metrics.emplace_back("settleFrames",
                         harness::JsonValue::integer(as_json_integer(stats_.settle_frames)));
    metrics.emplace_back("layoutRuns",
                         harness::JsonValue::integer(as_json_integer(host_.layout_runs())));
    metrics.emplace_back("layoutCacheHits",
                         harness::JsonValue::integer(as_json_integer(host_.layout_cache_hits())));
    metrics.emplace_back("emittedNodes",
                         harness::JsonValue::integer(as_json_integer(stats_.emitted_nodes)));

    harness::JsonValue::Array diagnostics_metrics;
    for (const diagnostics::MetricSample& sample : diagnostics().metrics->snapshot()) {
        diagnostics_metrics.push_back(harness::JsonValue::object(
            {{"name", harness::JsonValue::string(std::string(sample.name))},
             {"value", harness::JsonValue::integer(as_json_integer(sample.value))},
             {"kind", harness::JsonValue::string(sample.kind == diagnostics::MetricKind::Counter
                                                     ? "counter"
                                                     : "gauge")}}));
    }

    const bool ok = score >= 90.0;
    harness::JsonValue::Object report;
    report.emplace_back(std::string(kAuditVersionKey), harness::JsonValue::integer(1));
    report.emplace_back("profile", harness::JsonValue::string(std::string(profile)));
    report.emplace_back("score", harness::JsonValue::number(score));
    report.emplace_back("passed", harness::JsonValue::boolean(ok));
    report.emplace_back("checks", harness::JsonValue::array(std::move(encoded_checks)));
    report.emplace_back("metrics", harness::JsonValue::object(std::move(metrics)));
    report.emplace_back("diagnostics", harness::JsonValue::array(std::move(diagnostics_metrics)));

    auto document = harness::to_json(harness::JsonValue::object(std::move(report)));
    if (!document.has_value()) {
        return unexpected(error(harness::ProtocolErrorCode::Internal,
                                "audit report could not be encoded", std::string(profile)));
    }
    ++stats_.audit_reports;
    return harness::AuditArtifact{std::move(*document), score, ok};
}

} // namespace tessera::automation
