// SPDX-License-Identifier: MIT
//
// AF1 (issue #173) entry point: `ServiceContext` (ADR-050 section 3) and the `Service` state
// machine (section 4). One executable per area, matching the repository's unit-test convention; the
// two test groups live in `appfw_service_context_tests.cpp` and `appfw_service_runner_tests.cpp`.

#include "appfw_test_services.hpp"

#include <iostream>

namespace appfw_test {

int run_context_tests();
int run_runner_tests();

} // namespace appfw_test

int main() {
    const int control_flow = appfw_test::run_context_tests();
    const int state_machine = appfw_test::run_runner_tests();
    (void)control_flow;
    (void)state_machine;

    if (appfw_test::g_failures != 0) {
        std::cerr << "appfw_service: " << appfw_test::g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "appfw_service: ok\n";
    return 0;
}
