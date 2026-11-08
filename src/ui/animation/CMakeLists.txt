# SPDX-License-Identifier: MIT

add_library(tessera_animation STATIC
    keyframes.cpp
    runtime.cpp)

target_include_directories(tessera_animation PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_animation PUBLIC tessera_style)

target_compile_features(tessera_animation PUBLIC cxx_std_23)
