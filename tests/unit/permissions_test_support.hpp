// SPDX-License-Identifier: MIT
//
// The shared harness of the permissions contract suite (issue #451, docs/specs/permissions-v1.md).
// Header-only: the suite is split across `permissions_tests.cpp` (the state model, coalescing and
// the declaration rule) and `permissions_edges_tests.cpp` (failures, cancellation, refresh and
// observation) because each TU has to stay inside the file-size budget, and the fixture is
// identical.

#pragma once

#include "tessera/appfw/permission_fake.hpp"
#include "tessera/appfw/permission_service.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace permissions_test {

namespace appfw = tessera::appfw;
namespace runtime = tessera::runtime;

// The failure count of the test binary. Inline, so the two TUs share one counter and one exit code.
inline int failures = 0;

inline bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
    return condition;
}

inline void note_case(std::string_view name) {
    std::cerr << "case: " << name << "\n";
}

// The whole deterministic stack for one case: a manual clock, a completion queue, the fake back end
// and the service over them.
struct Fixture {
    explicit Fixture(std::uint64_t declared = appfw::all_permissions_mask(),
                     std::size_t max_records = 16)
        : backend(completions, clock) {
        appfw::PermissionConfig config;
        config.declared = declared;
        config.max_records = max_records;
        service = std::make_unique<appfw::PermissionService>(config, backend, completions);
    }

    // Pumps until the completion queue is empty, delivering due prompts first. Bounded, so a bug in
    // the module shows up as a failing assertion rather than a hung test.
    void settle() {
        for (int round = 0; round < 8; ++round) {
            const std::size_t delivered = backend.deliver_due();
            const std::size_t ran = completions.run(16);
            if (delivered == 0 && ran == 0) {
                return;
            }
        }
    }

    // One whole request: submit, deliver the prompt after advancing the clock, drain.
    appfw::PermissionResult ask(appfw::Permission permission, std::uint32_t delay_ms = 0,
                                appfw::PermissionRequestOptions options = {}) {
        appfw::PermissionResult captured;
        bool called = false;
        const appfw::PermissionTicket ticket =
            service->submit(permission, options, runtime::CancellationToken{},
                            [&captured, &called](appfw::PermissionResult result) {
                                captured = std::move(result);
                                called = true;
                            });
        (void)ticket;
        if (delay_ms > 0) {
            clock.advance(static_cast<double>(delay_ms) / 1000.0);
        }
        settle();
        if (!called) {
            return appfw::PermissionResult{};
        }
        return captured;
    }

    runtime::ManualClock clock;
    runtime::CompletionQueue completions;
    appfw::FakePermissionBackend backend;
    std::unique_ptr<appfw::PermissionService> service;
};

} // namespace permissions_test
