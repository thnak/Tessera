# SPDX-License-Identifier: MIT

add_library(tessera_runtime STATIC
    async.cpp
    clock.cpp
    diagnostics.cpp
    fixed_step.cpp
    frame_pacer.cpp
    runtime_loop.cpp
    worker_executor.cpp
    system_clock.cpp)

target_include_directories(tessera_runtime PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_runtime PUBLIC cxx_std_23)

find_package(Threads REQUIRED)

target_link_libraries(tessera_runtime
    PUBLIC Threads::Threads
    PRIVATE tessera_hal_timer)
