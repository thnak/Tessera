# SPDX-License-Identifier: MIT

# The ordered memory-pressure entry point (issue #705, memory-model-v1 section 7). It depends on no
# backend and no cache: a cache registers three callbacks, so this library stays a pure policy and
# every cache can link it without a cycle.
add_library(tessera_memory_pressure STATIC
    memory_pressure.cpp)

target_include_directories(tessera_memory_pressure PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_memory_pressure PUBLIC cxx_std_23)

if (TESSERA_BUILD_TESTS)
    enable_testing()

    add_executable(memory_pressure_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/memory_pressure_tests.cpp)
    target_link_libraries(memory_pressure_tests PRIVATE tessera_memory_pressure)
    add_test(NAME memory_pressure COMMAND memory_pressure_tests)
endif()
