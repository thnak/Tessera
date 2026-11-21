# SPDX-License-Identifier: MIT

add_library(tessera_behavior STATIC
    focus_trap.cpp
    instance.cpp
    press_feedback.cpp
    registry.cpp)

target_include_directories(tessera_behavior PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_behavior PUBLIC tessera_scene)

target_compile_features(tessera_behavior PUBLIC cxx_std_23)

# The behavior seam (ADR-064 item 7, spike #658). Registered here rather than in the shared
# tests/unit registry, so a lane adding tests never edits a file another lane is working in.
if (TESSERA_BUILD_TESTS)
    enable_testing()

    add_executable(behavior_registry_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/behavior_registry_tests.cpp)
    target_link_libraries(behavior_registry_tests PRIVATE tessera_behavior)
    add_test(NAME behavior_registry COMMAND behavior_registry_tests)

    add_executable(press_feedback_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/press_feedback_tests.cpp)
    target_link_libraries(press_feedback_tests PRIVATE tessera_behavior)
    add_test(NAME press_feedback COMMAND press_feedback_tests)

    add_executable(focus_trap_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/focus_trap_tests.cpp)
    target_link_libraries(focus_trap_tests PRIVATE tessera_behavior)
    add_test(NAME focus_trap COMMAND focus_trap_tests)

    add_executable(behavior_allocation_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/behavior_allocation_tests.cpp)
    target_link_libraries(behavior_allocation_tests PRIVATE tessera_behavior)
    add_test(NAME behavior_allocation COMMAND behavior_allocation_tests)

    add_executable(behavior_instance_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/behavior_instance_tests.cpp)
    target_link_libraries(behavior_instance_tests PRIVATE tessera_behavior)
    add_test(NAME behavior_instance COMMAND behavior_instance_tests)
endif()
