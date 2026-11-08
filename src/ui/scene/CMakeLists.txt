# SPDX-License-Identifier: MIT

add_library(tessera_scene STATIC
    node.cpp
    event.cpp
    dirty_pass.cpp)

target_include_directories(tessera_scene PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_scene PUBLIC tessera_rhi)

target_compile_features(tessera_scene PUBLIC cxx_std_23)
