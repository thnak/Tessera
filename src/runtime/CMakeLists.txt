# SPDX-License-Identifier: MIT

add_library(tessera_runtime STATIC
    async.cpp
    clock.cpp
    diagnostics.cpp
    fixed_step.cpp
    frame_pacer.cpp
    runtime_loop.cpp
    system_clock.cpp)

target_include_directories(tessera_runtime PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_runtime PUBLIC cxx_std_23)

target_link_libraries(tessera_runtime PRIVATE tessera_hal_timer)
