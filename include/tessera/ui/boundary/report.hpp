// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/boundary/error.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

namespace tessera::ui::boundary {

// Boundary ids are stable for the lifetime of one registration and are never reused, so a stale
// async report is detected by comparing the id plus generation instead of dereferencing a pointer.
using BoundaryId = std::uint32_t;

// The implicit boundary that owns everything outside a registered boundary. It is handled by the
// root handler, not by a component boundary.
inline constexpr BoundaryId kRootBoundary = 0;

// Diagnostics codes of section 7. One catch produces exactly one report; `Caught` is also used for
// a catastrophic error that the root handler received.
enum class BoundaryCode : std::uint8_t {
    Caught,
    FallbackFailed,
    Loop,
    Reset,
    Async,
};

[[nodiscard]] std::string_view code_name(BoundaryCode code);

// One caught event. `boundary` is the boundary that handled it (kRootBoundary when the root
// handler took it), so a report is never anonymous.
struct Report {
    BoundaryCode code = BoundaryCode::Caught;
    BoundaryId boundary = kRootBoundary;
    std::string component; // diagnostics id of the boundary/component, never user content
    Error error;
    FailureSite site = FailureSite::Build;
    std::uint32_t retries = 0;
    std::uint64_t generation = 0;
    // Async report whose boundary or child was already gone: recorded for diagnostics only, with no
    // fallback change and no onError callback (section 6, no stale callback).
    bool dropped = false;
};

// The onError seam. Every catch reports exactly once through this interface; no code path returns
// "caught" without producing a report, which is what makes a silent catch impossible.
class ReportSink {
public:
    virtual ~ReportSink() = default;

    virtual void on_report(const Report& report) = 0;
};

// Default diagnostics sink for a boundary without onError: keeps a bounded ring so tests and crash
// breadcrumbs can observe the report without a logger dependency.
class MemoryReportSink final : public ReportSink {
public:
    explicit MemoryReportSink(std::size_t capacity = 32);

    void on_report(const Report& report) override;
    void clear();

    // Number of retained reports with this code; the first thing a test or a crash breadcrumb asks.
    [[nodiscard]] std::size_t count(BoundaryCode code) const;

    [[nodiscard]] const std::deque<Report>& reports() const {
        return reports_;
    }
    [[nodiscard]] std::size_t dropped() const {
        return dropped_;
    }
    [[nodiscard]] std::size_t capacity() const {
        return capacity_;
    }

private:
    std::deque<Report> reports_;
    std::size_t capacity_ = 0;
    std::size_t dropped_ = 0;
};

// Builds the one report an event produces. Redacts home paths and caps code/message, so the onError
// path never sees a raw absolute path or unbounded text (section 7).
[[nodiscard]] Report make_report(BoundaryCode code, BoundaryId boundary, std::string component,
                                 Error error, FailureSite site, std::uint32_t retries,
                                 std::uint64_t generation);

} // namespace tessera::ui::boundary
