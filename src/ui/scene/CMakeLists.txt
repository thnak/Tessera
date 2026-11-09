# SPDX-License-Identifier: MIT

add_library(tessera_scene STATIC
    node.cpp
    event.cpp
    dirty_pass.cpp
    focus.cpp
    hover_dispatch.cpp
    input_dispatch.cpp
    text_input_dispatch.cpp)

target_include_directories(tessera_scene PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_scene PUBLIC tessera_rhi tessera_input)

target_compile_features(tessera_scene PUBLIC cxx_std_23)
