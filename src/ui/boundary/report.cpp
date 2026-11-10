// SPDX-License-Identifier: MIT

#include "tessera/diagnostics/log.hpp"
#include "tessera/ui/boundary/report.hpp"

#include <algorithm>
#include <utility>

namespace tessera::ui::boundary {

std::string_view code_name(BoundaryCode code) {
    switch (code) {
    case BoundaryCode::Caught:
        return "TESSERA_BOUNDARY_CAUGHT";
    case BoundaryCode::Cancelled:
        return "TESSERA_BOUNDARY_CANCELLED";
    case BoundaryCode::FallbackFailed:
        return "TESSERA_BOUNDARY_FALLBACK_FAILED";
    case BoundaryCode::Loop:
        return "TESSERA_BOUNDARY_LOOP";
    case BoundaryCode::Reset:
        return "TESSERA_BOUNDARY_RESET";
    case BoundaryCode::Async:
        return "TESSERA_BOUNDARY_ASYNC";
    }
    return "TESSERA_BOUNDARY_CAUGHT";
}

MemoryReportSink::MemoryReportSink(std::size_t capacity)
    : capacity_(std::max<std::size_t>(1, capacity)) {}

void MemoryReportSink::on_report(const Report& report) {
    if (reports_.size() >= capacity_) {
        reports_.pop_front();
        ++dropped_;
    }
    reports_.push_back(report);
}

void MemoryReportSink::clear() {
    reports_.clear();
    dropped_ = 0;
}

std::size_t MemoryReportSink::count(BoundaryCode code) const {
    std::size_t total = 0;
    for (const Report& report : reports_) {
        if (report.code == code) {
            ++total;
        }
    }
    return total;
}

Report make_report(BoundaryCode code, BoundaryId boundary, std::string component, Error error,
                   FailureSite site, std::uint32_t retries, std::uint64_t generation) {
    Report report;
    report.code = code;
    report.boundary = boundary;
    report.component = std::move(component);
    // Redaction happens before the report leaves the boundary, so neither onError nor a later crash
    // dump can leak a user home path (section 7). Bounding runs again because redaction is not a
    // length guarantee.
    error = bounded_error(std::move(error));
    error.message = diagnostics::redact_paths(error.message);
    report.error = bounded_error(std::move(error));
    report.site = site;
    report.retries = retries;
    report.generation = generation;
    return report;
}

} // namespace tessera::ui::boundary
