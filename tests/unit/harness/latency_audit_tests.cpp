// SPDX-License-Identifier: MIT
//
// Audit-mode latency gate. Pass/fail is decided on an injected fake clock, never the wall clock:
// evaluate_latency over hand-built reports (percentiles, thresholds, no-samples), the custom
// profile schema, and `tessera headless --mode audit` end to end with a fake clock that makes every
// step cost a fixed number of microseconds. Structural mode must keep its simulated clock.

#include "tessera/harness/cli.hpp"
#include "tessera/harness/json_write.hpp"
#include "tessera/harness/latency_audit.hpp"
#include "tessera/harness/session.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace tessera::harness;

constexpr std::string_view kFixtureDirectory = TESSERA_HARNESS_FIXTURE_SCRIPT_DIR;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "latency_audit: " << message << "\n";
        ++failures;
    }
    return condition;
}

// Every now_us() call advances by `step_us`, so a step that reads the clock twice costs exactly
// `step_us` and the audit result is a pure function of the constant.
class FakeAuditClock final : public SessionClock {
public:
    explicit FakeAuditClock(std::uint64_t step_us)
        : step_us_(step_us) {}
    [[nodiscard]] std::uint64_t now_us() const override {
        now_us_ += step_us_;
        return now_us_;
    }

private:
    std::uint64_t step_us_;
    mutable std::uint64_t now_us_ = 0;
};

ScriptReport report_with(const std::vector<std::uint64_t>& elapsed) {
    ScriptReport report;
    std::size_t index = 0;
    for (const std::uint64_t value : elapsed) {
        ScriptStepReport step;
        step.index = index++;
        step.action = ScriptAction::Click;
        step.ok = true;
        step.elapsed_us = value;
        report.steps.push_back(step);
    }
    return report;
}

struct Outcome {
    int code = 0;
    std::string out;
    std::string err;
};

Outcome run(const std::vector<std::string>& args, SessionClock* clock) {
    std::vector<std::string_view> views(args.begin(), args.end());
    std::ostringstream out;
    std::ostringstream err;
    Outcome outcome;
    outcome.code = run_cli_with_audit_clock(views, out, err, clock);
    outcome.out = out.str();
    outcome.err = err.str();
    return outcome;
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void test_evaluate() {
    const LatencyProfile profile{"t", 0, 90, 100};
    // Nearest rank over 10 samples: p50 is the 5th value, p95 the 10th.
    const LatencyAudit pass =
        evaluate_latency(report_with({10, 20, 30, 40, 50, 60, 70, 80, 90, 5}), profile);
    expect(pass.passed && pass.violations.empty(), "all within thresholds passes");
    expect(pass.stats.count == 10 && pass.stats.min_us == 5 && pass.stats.max_us == 90,
           "min, max, and count");
    expect(pass.stats.p50_us == 40 && pass.stats.p95_us == 90, "nearest-rank percentiles");

    const LatencyAudit slow = evaluate_latency(report_with({10, 200}), profile);
    expect(!slow.passed, "an over-limit sample fails");
    expect(slow.violations.size() == 2 && slow.violations[0].id == "p95-step" &&
               slow.violations[1].id == "max-step",
           "p95 and max violations are reported in order");
    expect(slow.violations[1].limit_us == 100 && slow.violations[1].actual_us == 200,
           "a violation carries limit and actual");

    const LatencyAudit edge = evaluate_latency(report_with({90}), profile);
    expect(edge.passed, "a value equal to the limit passes");

    ScriptReport failed_step = report_with({1});
    failed_step.steps[0].ok = false;
    ScriptStepReport snapshot;
    snapshot.action = ScriptAction::Snapshot;
    snapshot.ok = true;
    failed_step.steps.push_back(snapshot);
    const LatencyAudit none = evaluate_latency(failed_step, profile);
    expect(!none.passed && none.violations.size() == 1 && none.violations[0].id == "no-samples",
           "no successful input steps never passes vacuously");

    const std::string encoded = *to_json(encode_latency_audit(slow));
    expect(contains(encoded, "\"kind\":\"latency\"") && contains(encoded, "\"passed\":false") &&
               contains(encoded, "\"maxUs\":200"),
           "the audit document carries kind, verdict, and stats");
}

void test_profiles() {
    expect(builtin_latency_profile("desktop").has_value() &&
               builtin_latency_profile("mobile").has_value() &&
               builtin_latency_profile("ci").has_value(),
           "built-in profiles exist");
    expect(!builtin_latency_profile("nope").has_value(), "unknown names are rejected");

    const auto custom = parse_latency_profile(
        R"({"latencyProfileVersion":1,"name":"strict","p95StepUs":4000,"maxStepUs":9000})");
    expect(custom.has_value() && custom->name == "strict" && custom->p95_step_us == 4000 &&
               custom->max_step_us == 9000 && custom->p50_step_us == 0,
           "a custom profile parses");
    for (const char* bad : {
             R"({"name":"x","maxStepUs":1})",
             R"({"latencyProfileVersion":2,"name":"x","maxStepUs":1})",
             R"({"latencyProfileVersion":1,"maxStepUs":1})",
             R"({"latencyProfileVersion":1,"name":"x"})",
             R"({"latencyProfileVersion":1,"name":"x","maxStepUs":0})",
             R"({"latencyProfileVersion":1,"name":"x","maxStepUs":1,"extra":1})",
             R"([])",
         }) {
        expect(!parse_latency_profile(bad).has_value(), bad);
    }
    expect(!resolve_latency_profile("missing-profile").has_value(), "a bare unknown name fails");
    expect(!resolve_latency_profile("does-not-exist.json").has_value(), "a missing file fails");
}

void test_cli() {
    const std::string smoke = std::string(kFixtureDirectory) + "/smoke.json";
    std::error_code ignored;
    for (const char* directory : {"lat_ok", "lat_slow", "lat_struct", "lat_bad"}) {
        fs::remove_all(directory, ignored);
    }

    FakeAuditClock fast(5'000);
    const Outcome ok = run({"headless", "--mode", "audit", "--profile", "desktop", "--script",
                            smoke, "--out", "lat_ok"},
                           &fast);
    expect(ok.code == 0, "5 ms steps pass the desktop profile");
    expect(fs::exists("lat_ok/latency-audit.json"), "the latency artifact is written");
    const std::string artifact = read_file("lat_ok/latency-audit.json");
    expect(contains(artifact, "\"passed\":true") && contains(artifact, "\"p95Us\":5000"),
           "the artifact holds the fake-clock percentiles");
    expect(contains(read_file("lat_ok/report.json"), "\"latencyAudit\""),
           "the report embeds the latency section");
    expect(fs::exists("lat_ok/audit.json"), "the host audit artifact is still written");

    FakeAuditClock slow(60'000);
    const Outcome bad = run({"headless", "--mode", "audit", "--profile", "desktop", "--script",
                             smoke, "--out", "lat_slow"},
                            &slow);
    expect(bad.code == 1, "60 ms steps violate the desktop profile: exit 1");
    expect(contains(bad.err, "latency violation max-step"), "stderr names the violation");
    expect(contains(read_file("lat_slow/latency-audit.json"), "\"passed\":false"),
           "the artifact records the failure");
    expect(fs::exists("lat_slow/report.json"), "a violating run still writes its report");

    FakeAuditClock slow_again(60'000);
    const Outcome relaxed = run(
        {"headless", "--mode", "audit", "--profile", "ci", "--script", smoke, "--out", "lat_slow"},
        &slow_again);
    expect(relaxed.code == 0, "the same run passes the looser ci profile");

    FakeAuditClock unused(60'000);
    const Outcome structural = run({"test", "--script", smoke, "--out", "lat_struct"}, &unused);
    expect(structural.code == 0, "structural mode ignores the audit clock");
    expect(!fs::exists("lat_struct/latency-audit.json"), "structural mode writes no latency audit");
    expect(!contains(read_file("lat_struct/report.json"), "latencyAudit"),
           "structural reports have no latency section");

    FakeAuditClock never(1);
    const Outcome unknown = run({"headless", "--mode", "audit", "--profile", "bogus", "--script",
                                 smoke, "--out", "lat_bad"},
                                &never);
    expect(unknown.code == 2, "an unknown latency profile is invalid config");
    expect(!fs::exists("lat_bad/report.json"), "no run happens for an invalid profile");

    for (const char* directory : {"lat_ok", "lat_slow", "lat_struct", "lat_bad"}) {
        fs::remove_all(directory, ignored);
    }
}

} // namespace

int main() {
    test_evaluate();
    test_profiles();
    test_cli();
    if (failures != 0) {
        std::cerr << "latency_audit: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "latency_audit: ok\n";
    return 0;
}
