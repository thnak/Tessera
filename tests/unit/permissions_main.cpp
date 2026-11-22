// SPDX-License-Identifier: MIT
//
// The driver of the permissions contract suite (issue #451, docs/specs/permissions-v1.md). The
// cases live in two TUs - the state model, coalescing and the declaration rule in
// `permissions_tests.cpp`; failures, cancellation, refresh and observation in
// `permissions_edges_tests.cpp` - and this one runs both, so the suite is one executable and one
// exit code.
//
// The order is the spec's, and it is deliberate: a failure in the state model makes every later
// assertion meaningless, so it should be the first line of output, not buried after an edge case.

#include "permissions_test_support.hpp"

#include <iostream>
#include <string_view>

namespace permissions_test {
// Defined in `permissions_tests.cpp` and `permissions_edges_tests.cpp`.
void run_permissions_core_cases();
void run_permissions_edge_cases();
} // namespace permissions_test

namespace {

void note(std::string_view name) {
    std::cerr << "== " << name << "\n";
}

} // namespace

int main() {
    note("core: state model, coalescing, declaration");
    permissions_test::run_permissions_core_cases();

    note("edges: failures, cancellation, refresh, observation");
    permissions_test::run_permissions_edge_cases();

    if (permissions_test::failures != 0) {
        std::cerr << "permissions: " << permissions_test::failures << " failure(s)\n";
        return 1;
    }
    std::cout << "permissions: ok\n";
    return 0;
}
