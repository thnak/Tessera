# SPDX-License-Identifier: MIT

# Window size classes and the environment consumer set (issue #692): the coarse
# compact/medium/expanded buckets `useBreakpoint()` returns and the stylesheet's breakpoint tokens
# are written against.
#
# **Header-only.** The preview's session and every canvas test target compile the session sources
# directly rather than linking a library, so an out-of-line definition would need this target on a
# dozen link lines. The whole computation is two comparisons and the set is a vector of three-word
# records, so inlining costs nothing and keeps the dependency graph honest.
add_library(tessera_responsive INTERFACE)

target_include_directories(tessera_responsive INTERFACE
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_responsive INTERFACE
    tessera_style)

target_compile_features(tessera_responsive INTERFACE cxx_std_23)

# Registered here rather than in the shared tests/unit registry, so a lane adding tests never edits a
# file another lane is working in.
if (TESSERA_BUILD_TESTS)
    enable_testing()

    add_executable(size_class_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/size_class_tests.cpp)
    target_link_libraries(size_class_tests PRIVATE tessera_responsive)
    add_test(NAME size_class COMMAND size_class_tests)

    # The host half of `useBreakpoint()` (issue #692): the class from the media context, pushed to
    # the registered consumers after a size change, and nothing pushed inside one class.
    add_executable(breakpoint_host_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/breakpoint_host_tests.cpp)
    target_link_libraries(breakpoint_host_tests PRIVATE tessera_app tessera_responsive
        tessera_runtime)
    add_test(NAME breakpoint_host COMMAND breakpoint_host_tests)
endif ()
