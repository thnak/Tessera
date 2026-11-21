# SPDX-License-Identifier: MIT

add_library(tessera_behavior STATIC
    clipboard_copy.cpp
    focus_trap.cpp
    hotkeys.cpp
    instance.cpp
    overflow.cpp
    press_feedback.cpp
    registry.cpp
    roving_focus.cpp
    scroll_lock.cpp)

target_include_directories(tessera_behavior PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_behavior PUBLIC tessera_scene tessera_layout)

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

    add_executable(roving_focus_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/roving_focus_tests.cpp)
    target_link_libraries(roving_focus_tests PRIVATE tessera_behavior)
    add_test(NAME roving_focus COMMAND roving_focus_tests)

    add_executable(scroll_lock_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/scroll_lock_tests.cpp)
    target_link_libraries(scroll_lock_tests PRIVATE tessera_behavior)
    add_test(NAME scroll_lock COMMAND scroll_lock_tests)

    add_executable(hotkeys_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/hotkeys_tests.cpp)
    target_link_libraries(hotkeys_tests PRIVATE tessera_behavior)
    add_test(NAME hotkeys COMMAND hotkeys_tests)

    add_executable(clipboard_copy_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/clipboard_copy_tests.cpp)
    target_link_libraries(clipboard_copy_tests PRIVATE tessera_behavior tessera_hal_clipboard)
    add_test(NAME clipboard_copy COMMAND clipboard_copy_tests)

    add_executable(overflow_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/overflow_tests.cpp)
    target_link_libraries(overflow_tests PRIVATE tessera_behavior tessera_layout)
    add_test(NAME overflow COMMAND overflow_tests)

    add_executable(behavior_allocation_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/behavior_allocation_tests.cpp)
    target_link_libraries(behavior_allocation_tests PRIVATE tessera_behavior)
    add_test(NAME behavior_allocation COMMAND behavior_allocation_tests)

    add_executable(behavior_instance_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/behavior_instance_tests.cpp)
    target_link_libraries(behavior_instance_tests PRIVATE tessera_behavior)
    add_test(NAME behavior_instance COMMAND behavior_instance_tests)
endif()
