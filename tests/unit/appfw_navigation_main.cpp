// SPDX-License-Identifier: MIT
//
// The navigation module's entry point (issue #196). One executable per area, matching the
// repository's unit-test convention, with one TU per acceptance word so a failure names the group
// it belongs to:
//
//   * appfw_navigation_route_tests.cpp     - push/replace, params, unknown route
//   * appfw_navigation_back_tests.cpp      - pop, modal/sheet priority, empty stack, root edge
//   * appfw_navigation_deep_link_tests.cpp - cold/warm arrival, allowlists, malformed input
//   * appfw_navigation_restore_tests.cpp   - round trip, version/table gates, persistence through
//   storage
//
// Every group is deterministic: explicit route tables, a `ManualIoExecutor` and an in-memory file
// system, and an `UpdateScheduler` the test flushes by hand. No wall clock, no OS, no network, no
// file.

#include "appfw_navigation_test_support.hpp"

#include <iostream>

namespace nav_test {

int run_route_tests();
int run_back_tests();
int run_deep_link_tests();
int run_restore_tests();

} // namespace nav_test

int main() {
    const int route = nav_test::run_route_tests();
    const int back = nav_test::run_back_tests();
    const int deep_link = nav_test::run_deep_link_tests();
    const int restore = nav_test::run_restore_tests();
    (void)route;
    (void)back;
    (void)deep_link;
    (void)restore;

    if (nav_test::g_failures != 0) {
        std::cerr << "appfw_navigation: " << nav_test::g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "appfw_navigation: ok\n";
    return 0;
}
