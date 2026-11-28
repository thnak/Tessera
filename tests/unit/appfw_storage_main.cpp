// SPDX-License-Identifier: MIT
//
// AF7 (issue #179) entry point. One executable per area, matching the repository's unit-test
// convention; the groups live in the sibling TUs:
//
//   * appfw_storage_atomic_tests.cpp     - the atomic-replace sequence and induced mid-write
//   failure
//   * appfw_storage_cancel_tests.cpp     - cancellation, temp-file cleanup, no corruption
//   * appfw_storage_corruption_tests.cpp - corruption fallback and its diagnostic
//   * appfw_storage_executor_tests.cpp   - the executor seam and the settings entry point
//   * appfw_storage_migration_tests.cpp  - the migration chain and load_migrated (#411)
//   * appfw_storage_cache_tests.cpp      - the bounded LRU cache store (#411)
//
// Every group is deterministic: `ManualIoExecutor`, a fake `FileSystem`, no wall clock, no sleep
// and no thread. The one real-thread case is in `appfw_storage_executor_tests.cpp` and exists only
// to prove the seam (`WorkerIoExecutor`) actually runs work off the calling thread.

#include "appfw_storage_test_support.hpp"

#include <iostream>

namespace storage_test {

int run_atomic_tests();
int run_cancel_tests();
int run_corruption_tests();
int run_executor_tests();
int run_migration_tests();
int run_cache_tests();

} // namespace storage_test

int main() {
    const int atomic = storage_test::run_atomic_tests();
    const int cancel = storage_test::run_cancel_tests();
    const int corruption = storage_test::run_corruption_tests();
    const int executor = storage_test::run_executor_tests();
    const int migration = storage_test::run_migration_tests();
    const int cache = storage_test::run_cache_tests();
    (void)migration;
    (void)cache;
    (void)atomic;
    (void)cancel;
    (void)corruption;
    (void)executor;

    if (storage_test::g_failures != 0) {
        std::cerr << "appfw_storage: " << storage_test::g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "appfw_storage: ok\n";
    return 0;
}
