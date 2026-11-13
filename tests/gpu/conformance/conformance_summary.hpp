// SPDX-License-Identifier: MIT
//
// The conformance summary (issue #161): a deterministic JSON artifact reporting pass/skip/fail per
// check for one backend. Formatting is pure and sorts check names, so two runs with the same
// results are byte-identical; nothing here records a timestamp, a pointer or a path.

#pragma once

#include "conformance_check.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace tessera::tests::gpu::conformance {

struct CheckOutcome {
    std::string name;
    Capability capability = Capability::Device;
    Status status = Status::Pass;
    std::string detail;
};

struct Summary {
    std::string backend;
    // Declaration/run order; format_summary sorts a copy by name.
    std::vector<CheckOutcome> checks;
};

struct SummaryTotals {
    std::size_t pass = 0;
    std::size_t skip = 0;
    std::size_t fail = 0;

    [[nodiscard]] std::size_t total() const {
        return pass + skip + fail;
    }
};

[[nodiscard]] SummaryTotals totals(const Summary& summary);
[[nodiscard]] bool has_failure(const Summary& summary);

// Canonical JSON (schema 1): object keys and check entries sorted, two-space indentation, LF line
// endings, one trailing newline. Deterministic for identical summaries.
[[nodiscard]] std::string format_summary(const Summary& summary);

} // namespace tessera::tests::gpu::conformance
