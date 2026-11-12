// SPDX-License-Identifier: MIT

#include "tessera/harness/json_parse.hpp"
#include "tessera/harness/latency_audit.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <utility>

namespace tessera::harness {
namespace {

constexpr std::size_t kMaxProfileBytes = 4096;

std::int64_t as_json_integer(std::uint64_t value) noexcept {
    constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return static_cast<std::int64_t>(std::min(value, kMax));
}

JsonValue integer(std::uint64_t value) {
    return JsonValue::integer(as_json_integer(value));
}

bool is_input_action(ScriptAction action) noexcept {
    return action == ScriptAction::Click || action == ScriptAction::Type ||
           action == ScriptAction::Focus || action == ScriptAction::Scroll ||
           action == ScriptAction::ScrollIntoView;
}

// Nearest rank: the smallest value with at least `percent` percent of samples at or below it.
std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, std::size_t percent) {
    const std::size_t rank = (percent * sorted.size() + 99) / 100;
    return sorted[rank == 0 ? 0 : rank - 1];
}

void check(LatencyAudit& audit, std::string id, std::uint64_t limit, std::uint64_t actual) {
    if (limit != 0 && actual > limit) {
        audit.violations.push_back(LatencyViolation{std::move(id), limit, actual});
    }
}

} // namespace

std::optional<LatencyProfile> builtin_latency_profile(std::string_view name) {
    if (name == "desktop") {
        return LatencyProfile{"desktop", 8'000, 16'000, 50'000};
    }
    if (name == "mobile") {
        return LatencyProfile{"mobile", 16'000, 33'000, 100'000};
    }
    if (name == "ci") {
        return LatencyProfile{"ci", 0, 250'000, 1'000'000};
    }
    return std::nullopt;
}

std::expected<LatencyProfile, std::string> parse_latency_profile(std::string_view json) {
    auto parsed = parse_json(json);
    if (!parsed.has_value() || !parsed->is_object()) {
        return std::unexpected("latency profile is not a JSON object");
    }
    LatencyProfile profile;
    bool has_version = false;
    for (const auto& [key, value] : *parsed->as_object()) {
        if (key == "latencyProfileVersion") {
            if (value.as_integer() != 1) {
                return std::unexpected("unsupported latencyProfileVersion");
            }
            has_version = true;
        } else if (key == "name") {
            const auto text = value.as_string();
            if (!text.has_value() || text->empty() || text->size() > 64) {
                return std::unexpected("name must be a non-empty string of at most 64 bytes");
            }
            profile.name = std::string(*text);
        } else if (key == "p50StepUs" || key == "p95StepUs" || key == "maxStepUs") {
            const auto number = value.as_integer();
            if (!number.has_value() || *number <= 0) {
                return std::unexpected(key + " must be a positive integer");
            }
            auto& target = key == "p50StepUs"   ? profile.p50_step_us
                           : key == "p95StepUs" ? profile.p95_step_us
                                                : profile.max_step_us;
            target = static_cast<std::uint64_t>(*number);
        } else {
            return std::unexpected("unknown latency profile member '" + key + "'");
        }
    }
    if (!has_version) {
        return std::unexpected("latencyProfileVersion is required");
    }
    if (profile.name.empty()) {
        return std::unexpected("name is required");
    }
    if (profile.p50_step_us == 0 && profile.p95_step_us == 0 && profile.max_step_us == 0) {
        return std::unexpected("at least one threshold must be set");
    }
    return profile;
}

std::expected<LatencyProfile, std::string> resolve_latency_profile(std::string_view arg) {
    if (auto builtin = builtin_latency_profile(arg); builtin.has_value()) {
        return *builtin;
    }
    if (!arg.ends_with(".json")) {
        return std::unexpected("unknown latency profile '" + std::string(arg) +
                               "' (use desktop, mobile, ci, or a .json profile file)");
    }
    std::ifstream file(std::string(arg), std::ios::binary);
    if (!file) {
        return std::unexpected("cannot read latency profile '" + std::string(arg) + "'");
    }
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (text.size() > kMaxProfileBytes) {
        return std::unexpected("latency profile exceeds the size cap");
    }
    return parse_latency_profile(text);
}

LatencyAudit evaluate_latency(const ScriptReport& report, LatencyProfile profile) {
    LatencyAudit audit;
    audit.profile = std::move(profile);
    std::vector<std::uint64_t> sorted;
    for (const ScriptStepReport& step : report.steps) {
        if (step.ok && is_input_action(step.action)) {
            audit.samples.push_back(LatencySample{step.index, step.action, step.elapsed_us});
            sorted.push_back(step.elapsed_us);
        }
    }
    if (sorted.empty()) {
        audit.violations.push_back(LatencyViolation{"no-samples", 0, 0});
        return audit;
    }
    std::sort(sorted.begin(), sorted.end());
    audit.stats = LatencyStats{sorted.size(), sorted.front(), percentile(sorted, 50),
                               percentile(sorted, 95), sorted.back()};
    check(audit, "p50-step", audit.profile.p50_step_us, audit.stats.p50_us);
    check(audit, "p95-step", audit.profile.p95_step_us, audit.stats.p95_us);
    check(audit, "max-step", audit.profile.max_step_us, audit.stats.max_us);
    audit.passed = audit.violations.empty();
    return audit;
}

JsonValue encode_latency_audit(const LatencyAudit& audit) {
    JsonValue::Object thresholds;
    thresholds.emplace_back("p50StepUs", integer(audit.profile.p50_step_us));
    thresholds.emplace_back("p95StepUs", integer(audit.profile.p95_step_us));
    thresholds.emplace_back("maxStepUs", integer(audit.profile.max_step_us));

    JsonValue::Object stats;
    stats.emplace_back("minUs", integer(audit.stats.min_us));
    stats.emplace_back("p50Us", integer(audit.stats.p50_us));
    stats.emplace_back("p95Us", integer(audit.stats.p95_us));
    stats.emplace_back("maxUs", integer(audit.stats.max_us));

    JsonValue::Array violations;
    for (const LatencyViolation& violation : audit.violations) {
        violations.push_back(JsonValue::object({{"id", JsonValue::string(violation.id)},
                                                {"limitUs", integer(violation.limit_us)},
                                                {"actualUs", integer(violation.actual_us)}}));
    }
    JsonValue::Array samples;
    for (const LatencySample& sample : audit.samples) {
        samples.push_back(
            JsonValue::object({{"step", integer(sample.step)},
                               {"action", JsonValue::string(std::string(to_string(sample.action)))},
                               {"elapsedUs", integer(sample.elapsed_us)}}));
    }

    JsonValue::Object members;
    members.emplace_back("auditVersion", JsonValue::integer(1));
    members.emplace_back("kind", JsonValue::string("latency"));
    members.emplace_back("profile", JsonValue::string(audit.profile.name));
    members.emplace_back("passed", JsonValue::boolean(audit.passed));
    members.emplace_back("samples", integer(audit.stats.count));
    members.emplace_back("thresholds", JsonValue::object(std::move(thresholds)));
    members.emplace_back("stats", JsonValue::object(std::move(stats)));
    members.emplace_back("violations", JsonValue::array(std::move(violations)));
    members.emplace_back("steps", JsonValue::array(std::move(samples)));
    return JsonValue::object(std::move(members));
}

} // namespace tessera::harness
