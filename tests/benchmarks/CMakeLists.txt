# SPDX-License-Identifier: MIT

# GPU-free benchmark suites. The CPU suite is the V1 timing evidence for the single-threaded runtime
# decision (docs/design/performance.md section 9.1). The stylesheet suite is the deterministic
# startup evidence for the compiled-stylesheet blob (issue #201, ADR-048).
add_subdirectory(cpu)
add_subdirectory(stylesheet)

# The memory probe (issue #13) is not a timing benchmark: it prints the budgeted memory quantities
# for scripts/check_memory_budget.py and runs as the cheap `memory_sizes` CTest. It lives here so it
# shares this directory's "measured evidence, not a product" framing.
add_subdirectory(memory)
