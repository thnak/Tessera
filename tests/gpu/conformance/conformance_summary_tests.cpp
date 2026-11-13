// SPDX-License-Identifier: MIT
//
// Determinism and shape self-tests for the conformance summary (issue #161). The null suite runs
// twice on two independent devices and the two summaries must be byte-identical. The result must
// be a non-empty, sorted check list with no failures, skip reasons that name the missing
// capability, totals that count every check, and no machine-specific text (paths, timestamps).

#include "conformance_backend.hpp"
#include "conformance_suite.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace tessera::tests::gpu::conformance;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "conformance_summary: " << message << "\n";
    }
    return condition;
}

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

bool all_passed_or_skipped(const Summary& summary) {
    for (const CheckOutcome& outcome : summary.checks) {
        if (outcome.status == Status::Fail) {
            return false;
        }
        if (outcome.status == Status::Skip) {
            if (outcome.detail.rfind("missing capability: ", 0) != 0) {
                std::cerr << "conformance_summary: skip reason for " << outcome.name
                          << " does not name a capability: " << outcome.detail << "\n";
                return false;
            }
        }
    }
    return true;
}

} // namespace

int main() {
    OpenedBackend first_open = open_backend(BackendChoice::Null);
    if (!expect(first_open.available(), "the null backend did not open")) {
        return 1;
    }
    const Summary first = run_suite(*first_open.device, first_open.profile, "null");

    OpenedBackend second_open = open_backend(BackendChoice::Null);
    if (!expect(second_open.available(), "the null backend did not open twice")) {
        return 1;
    }
    const Summary second = run_suite(*second_open.device, second_open.profile, "null");

    const std::string first_json = format_summary(first);
    const std::string second_json = format_summary(second);
    if (!expect(first_json == second_json,
                "two null-conformance runs produced different summaries")) {
        std::cerr << "first:\n" << first_json << "second:\n" << second_json;
        return 2;
    }

    if (!expect(!first.checks.empty(), "the null suite ran no checks")) {
        return 3;
    }
    if (!expect(all_passed_or_skipped(first), "the null suite must be all pass or skip")) {
        return 4;
    }
    if (!expect(contains(first_json, "\"status\": \"pass\""), "no check passed")) {
        return 5;
    }
    if (!expect(contains(first_json, "\"backend\": \"null\""), "the backend name is missing")) {
        return 6;
    }
    if (!expect(contains(first_json, "\"total\": " + std::to_string(first.checks.size())),
                "the totals do not count every check")) {
        return 7;
    }
    if (!expect(first_json.find('\\') == std::string::npos,
                "the summary contains a backslash path separator")) {
        return 8;
    }
    if (!expect(first_json.find('/') == std::string::npos,
                "the summary contains a path separator")) {
        return 9;
    }

    // Sorting and totals are a property of the formatter, not of the run order: feed it a
    // deliberately unsorted summary.
    Summary unsorted;
    unsorted.backend = "null";
    unsorted.checks = {
        {"zeta", Capability::Device, Status::Pass, ""},
        {"alpha", Capability::CommandList, Status::Skip, "missing capability: command_list"},
        {"mu", Capability::Texture, Status::Fail, "synthetic failure"},
    };
    const std::string unsorted_json = format_summary(unsorted);
    const std::size_t alpha = unsorted_json.find("\"name\": \"alpha\"");
    const std::size_t mu = unsorted_json.find("\"name\": \"mu\"");
    const std::size_t zeta = unsorted_json.find("\"name\": \"zeta\"");
    if (!expect(alpha != std::string::npos && alpha < mu && mu < zeta,
                "check names are not sorted in the summary")) {
        return 10;
    }
    if (!expect(contains(unsorted_json, "\"fail\": 1") && contains(unsorted_json, "\"pass\": 1") &&
                    contains(unsorted_json, "\"skip\": 1") &&
                    contains(unsorted_json, "\"total\": 3"),
                "the summary totals are wrong")) {
        return 11;
    }
    if (!expect(format_summary(unsorted) == unsorted_json, "formatting is not repeatable")) {
        return 12;
    }

    Summary quoted;
    quoted.backend = "null";
    quoted.checks = {{"quote\"check", Capability::Device, Status::Fail, "say \"hi\"\n"}};
    const std::string quoted_json = format_summary(quoted);
    if (!expect(contains(quoted_json, "quote\\\"check"), "a check name is not JSON-escaped")) {
        return 13;
    }
    if (!expect(contains(quoted_json, "say \\\"hi\\\"\\n"), "a detail is not JSON-escaped")) {
        return 14;
    }

    std::cout << "conformance_summary: ok (" << first.checks.size()
              << " checks, byte-identical reruns)\n";
    return 0;
}
