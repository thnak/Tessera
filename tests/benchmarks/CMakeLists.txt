# SPDX-License-Identifier: MIT

# GPU-free benchmark suites. The CPU suite is the V1 timing evidence for the single-threaded runtime
# decision (docs/design/performance.md section 9.1). The stylesheet suite is the deterministic
# startup evidence for the compiled-stylesheet blob (issue #201, ADR-048).
add_subdirectory(cpu)
add_subdirectory(stylesheet)
