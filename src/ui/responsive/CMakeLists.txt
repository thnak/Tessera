# SPDX-License-Identifier: MIT

# Window size classes (issue #692): the coarse compact/medium/expanded buckets `useBreakpoint()`
# returns and the stylesheet's breakpoint tokens are written against. It depends only on the media
# context, so the hook, the preview's `viewport.set` report and an app's own `@media` rules all read
# one answer.
add_library(tessera_responsive STATIC
    environment_set.cpp
    size_class.cpp)

target_include_directories(tessera_responsive PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_responsive PUBLIC
    tessera_style)

target_compile_features(tessera_responsive PUBLIC cxx_std_23)

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
