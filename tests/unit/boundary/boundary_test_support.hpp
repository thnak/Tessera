// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/boundary/boundary.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::boundary::test {

// The last catch point: records every report that bypassed or escaped a component boundary.
class RecordingRoot final : public RootHandler {
public:
    void on_root_error(const Report& report) override {
        reports.push_back(report);
    }

    [[nodiscard]] const Report* last() const {
        return reports.empty() ? nullptr : &reports.back();
    }

    [[nodiscard]] std::size_t count(BoundaryCode code) const {
        std::size_t total = 0;
        for (const Report& report : reports) {
            if (report.code == code) {
                ++total;
            }
        }
        return total;
    }

    std::vector<Report> reports;
};

// A boundary's onError channel.
class RecordingSink final : public ReportSink {
public:
    void on_report(const Report& report) override {
        reports.push_back(report);
    }

    [[nodiscard]] const Report* last() const {
        return reports.empty() ? nullptr : &reports.back();
    }

    [[nodiscard]] std::size_t count(BoundaryCode code) const {
        std::size_t total = 0;
        for (const Report& report : reports) {
            if (report.code == code) {
                ++total;
            }
        }
        return total;
    }

    std::vector<Report> reports;
};

// Records every cleanup call in order, so a leak or a double dispose is visible to the tests.
class RecordingOwner final : public SubtreeOwner {
public:
    void dispose_subtree(std::string_view subtree) override {
        disposed.emplace_back(subtree);
    }

    [[nodiscard]] std::size_t count(std::string_view subtree) const {
        std::size_t total = 0;
        for (const std::string& name : disposed) {
            if (name == subtree) {
                ++total;
            }
        }
        return total;
    }

    std::vector<std::string> disposed;
};

// An onError that violates its contract; the catch point must contain it, not propagate it.
class ThrowingSink final : public ReportSink {
public:
    void on_report(const Report&) override {
        throw std::runtime_error("onError threw");
    }
};

} // namespace tessera::ui::boundary::test
