// SPDX-License-Identifier: MIT

#include "conformance_suite.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace tessera::tests::gpu::conformance {
namespace {

CheckOutcome skipped(const Check& check, std::string reason) {
    CheckOutcome outcome;
    outcome.name = std::string(check.name);
    outcome.capability = check.capability;
    outcome.status = Status::Skip;
    outcome.detail = std::move(reason);
    return outcome;
}

} // namespace

Summary run_suite(rendering::rhi::Device& device, const BackendProfile& profile,
                  std::string backend) {
    std::vector<CheckOutcome> outcomes;
    const std::span<const Check> all = checks();
    outcomes.reserve(all.size());

    for (const Check& check : all) {
        if (!profile.has(check.capability)) {
            outcomes.push_back(skipped(check, "missing capability: " +
                                                  std::string(capability_name(check.capability))));
            continue;
        }
        if (check.run == nullptr) {
            CheckOutcome outcome;
            outcome.name = std::string(check.name);
            outcome.capability = check.capability;
            outcome.status = Status::Fail;
            outcome.detail = "the check has no run function";
            outcomes.push_back(std::move(outcome));
            continue;
        }

        CheckResult result;
        try {
            result = check.run(device);
        } catch (const std::exception& error) {
            // Driver messages can carry absolute paths; keep them out of the summary and surface
            // them on stderr for a developer instead.
            std::cerr << "gpu_conformance: check " << check.name << " threw: " << error.what()
                      << "\n";
            result = CheckResult::fail("the check threw an exception");
        } catch (...) {
            std::cerr << "gpu_conformance: check " << check.name << " threw an unknown exception\n";
            result = CheckResult::fail("the check threw an unknown exception");
        }

        CheckOutcome outcome;
        outcome.name = std::string(check.name);
        outcome.capability = check.capability;
        outcome.status = result.status;
        outcome.detail = std::move(result.detail);
        outcomes.push_back(std::move(outcome));
    }

    Summary summary;
    summary.backend = std::move(backend);
    summary.checks = std::move(outcomes);
    return summary;
}

Summary unavailable_suite(std::string backend) {
    std::vector<CheckOutcome> outcomes;
    const std::span<const Check> all = checks();
    outcomes.reserve(all.size());
    for (const Check& check : all) {
        outcomes.push_back(skipped(check, "missing capability: device"));
    }

    Summary summary;
    summary.backend = std::move(backend);
    summary.checks = std::move(outcomes);
    return summary;
}

} // namespace tessera::tests::gpu::conformance
