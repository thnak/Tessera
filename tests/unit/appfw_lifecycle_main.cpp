// SPDX-License-Identifier: MIT
//
// AF3 (issue #175) entry point: the app lifecycle (ADR-050 section 2). One executable per area,
// matching the repository's unit-test convention; the state table and the script decoder live in
// `appfw_lifecycle_tests.cpp`, the `Application` behaviour in `appfw_application_tests.cpp`, and
// the window set of issue #180 in `appfw_window_registry_tests.cpp`.
//
// Each group returns its own failure count, so the exit code is the total and a group that fails
// early cannot hide behind another group's counter.

#include <iostream>

int run_lifecycle_tests();
int run_application_tests();
int run_lifecycle_script_tests();
int run_lifecycle_seam_tests();
int run_window_registry_tests();

int main() {
    const int failures = run_lifecycle_tests() + run_application_tests() +
                         run_lifecycle_script_tests() + run_lifecycle_seam_tests() +
                         run_window_registry_tests();
    if (failures != 0) {
        std::cerr << "appfw_lifecycle: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "appfw_lifecycle: ok\n";
    return 0;
}
