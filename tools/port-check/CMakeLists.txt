# SPDX-License-Identifier: MIT

add_executable(tessera_port_check ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)

target_include_directories(tessera_port_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# `tessera` is the name ADR-058 gives the command (`tessera port-check`), and the binary is the one
# spelling of it today: the CLI has no subcommand dispatcher yet, so the executable carries the
# bare name and the usage text says which subcommand it is.
set_target_properties(tessera_port_check PROPERTIES OUTPUT_NAME tessera)

target_compile_features(tessera_port_check PRIVATE cxx_std_23)

# The checker's own sources, as a library, so the test can link them without linking an executable
# (CMake refuses that, and duplicating the file list would let the two drift).
add_library(tessera_port_check_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/port_check.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/class_table.cpp)
target_include_directories(tessera_port_check_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(tessera_port_check_core PUBLIC cxx_std_23)
target_link_libraries(tessera_port_check PRIVATE tessera_port_check_core)

if (TESSERA_BUILD_TESTS)
    add_executable(port_check_tests ${CMAKE_CURRENT_SOURCE_DIR}/port_check_tests.cpp)
    target_link_libraries(port_check_tests PRIVATE tessera_port_check_core)
    add_test(NAME port_check COMMAND port_check_tests)
endif ()
