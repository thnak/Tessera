# SPDX-License-Identifier: MIT

add_library(tessera_runtime STATIC
    adaptive_quality.cpp
    async.cpp
    clock.cpp
    config.cpp
    config_json.cpp
    device_recovery.cpp
    diagnostics.cpp
    fixed_step.cpp
    frame_pacer.cpp
    io_executor.cpp
    present_cap.cpp
    runtime_loop.cpp
    task.cpp
    worker_executor.cpp
    system_clock.cpp)

target_include_directories(tessera_runtime PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_runtime PUBLIC cxx_std_23)

find_package(Threads REQUIRED)

target_link_libraries(tessera_runtime
    PUBLIC Threads::Threads tessera_hal_window
    PRIVATE tessera_hal_timer)
