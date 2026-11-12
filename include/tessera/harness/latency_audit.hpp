// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/json_value.hpp"
#include "tessera/harness/script_runner.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::harness {

// Audit-mode latency gate (docs/specs/headless-automation-v1.md section 6.10). The harness only
// evaluates numbers: where they come from is the injected SessionClock the ScriptRunner used, so
// audit mode passes a monotonic clock and tests pass a fake one. Structural and golden runs never
// build a LatencyAudit.
//
// A threshold of 0 means "not checked". Units are microseconds of per-step elapsed time for steps
// that go through the host's input path (click/type/focus/scroll): dispatch, input delivery, and
// the settle frame.
struct LatencyProfile {
    std::string name;
    std::uint64_t p50_step_us = 0;
    std::uint64_t p95_step_us = 0;
    std::uint64_t max_step_us = 0;
};

inline constexpr std::string_view kLatencyAuditFile = "latency-audit.json";

// Built-in profiles: "desktop", "mobile", and "ci" (loose, for noisy shared runners).
[[nodiscard]] std::optional<LatencyProfile> builtin_latency_profile(std::string_view name);

// Custom profile document: {"latencyProfileVersion":1,"name":"x","p50StepUs":N,"p95StepUs":N,
// "maxStepUs":N}. Thresholds are optional, at least one must be set, unknown members are rejected.
[[nodiscard]] std::expected<LatencyProfile, std::string>
parse_latency_profile(std::string_view json);

// A built-in name, or a path ending in ".json" read as a custom profile document.
[[nodiscard]] std::expected<LatencyProfile, std::string>
resolve_latency_profile(std::string_view arg);

struct LatencySample {
    std::size_t step = 0;
    ScriptAction action = ScriptAction::Click;
    std::uint64_t elapsed_us = 0;
};

struct LatencyStats {
    std::size_t count = 0;
    std::uint64_t min_us = 0;
    std::uint64_t p50_us = 0;
    std::uint64_t p95_us = 0;
    std::uint64_t max_us = 0;
};

struct LatencyViolation {
    std::string id; // "p50-step", "p95-step", "max-step", or "no-samples"
    std::uint64_t limit_us = 0;
    std::uint64_t actual_us = 0;
};

struct LatencyAudit {
    LatencyProfile profile;
    std::vector<LatencySample> samples;
    LatencyStats stats;
    std::vector<LatencyViolation> violations;
    bool passed = false;
};

// Nearest-rank percentiles over the successful input steps of `report`. A run with no such step
// has nothing to gate on and fails with "no-samples" rather than passing vacuously.
[[nodiscard]] LatencyAudit evaluate_latency(const ScriptReport& report, LatencyProfile profile);

[[nodiscard]] JsonValue encode_latency_audit(const LatencyAudit& audit);

} // namespace tessera::harness
