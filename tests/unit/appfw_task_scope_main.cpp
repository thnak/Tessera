// SPDX-License-Identifier: MIT
//
// AF2 (issue #174) entry point: `appfw::TaskScope` (ADR-050 section 5 item 1). Three test groups,
// each its own translation unit and each returning its own failure count, so a group that fails
// early cannot hide behind another group's counter:
//
//   * `appfw_task_scope_tests.cpp`              - close/cancel/drain, finished children, capacity
//   * `appfw_task_scope_suspension_tests.cpp`   - destruction while suspended, the close deadline,
//                                                 nesting
//   * `appfw_task_scope_cancel_tests.cpp`       - the runtime's TaskCancelled path, refused posts,
//                                                 a throwing child
//
// The layer-0 timer wake source has its own executable (`runtime_timer_tests`, ADR-050 section 5
// item 2) because it is a layer-0 target that links `tessera_runtime` only; keeping them separate
// keeps the dependency direction visible in the build.

#include <iostream>

namespace appfw_task_test {
int run_lifecycle_tests();
int run_suspension_tests();
int run_cancel_tests();
} // namespace appfw_task_test

int main() {
    const int failures = appfw_task_test::run_lifecycle_tests() +
                         appfw_task_test::run_suspension_tests() +
                         appfw_task_test::run_cancel_tests();
    if (failures != 0) {
        std::cerr << "appfw_task_scope: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "appfw_task_scope: ok\n";
    return 0;
}
