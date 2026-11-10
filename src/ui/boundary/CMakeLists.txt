# SPDX-License-Identifier: MIT

# Error boundaries (`docs/specs/error-boundaries-v1.md`). The slice owns its targets here so
# parallel slices never edit the root subdirectory list at the same time.

add_library(tessera_boundary STATIC
    boundary.cpp
    boundary_catch.cpp
    error.cpp
    guard.cpp
    report.cpp
    retry_policy.cpp)

target_include_directories(tessera_boundary PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

# Report redaction reuses the diagnostics path redactor instead of growing a second one (ADR-022).
target_link_libraries(tessera_boundary PUBLIC
    tessera_diagnostics)

target_compile_features(tessera_boundary PUBLIC cxx_std_23)
