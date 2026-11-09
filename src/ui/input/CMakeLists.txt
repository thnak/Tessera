# SPDX-License-Identifier: MIT

add_library(tessera_input STATIC
    input_queue.cpp)

target_include_directories(tessera_input PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_input PUBLIC cxx_std_23)

# The pump is a UI-thread convenience layer over the queue, scene router, and runtime loop. It is a
# separate target so tessera_input stays usable by the platform translators without a scene/runtime
# dependency.
add_library(tessera_input_pump STATIC
    input_pump.cpp)

target_include_directories(tessera_input_pump PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_input_pump PUBLIC
    tessera_input
    tessera_scene
    tessera_runtime)

target_compile_features(tessera_input_pump PUBLIC cxx_std_23)
