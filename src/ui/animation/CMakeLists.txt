# SPDX-License-Identifier: MIT

add_library(tessera_animation STATIC
    easing.cpp
    keyframes.cpp
    property_dirty.cpp
    runtime.cpp
    spring.cpp
    spring_runtime.cpp
    style_overlay.cpp
    transition.cpp
    animation_shorthand.cpp
    animation_start.cpp
    animation_style.cpp
    transition_runtime.cpp)

target_include_directories(tessera_animation PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_animation PUBLIC tessera_style)

target_compile_features(tessera_animation PUBLIC cxx_std_23)

add_library(tessera_animation_loop STATIC
    loop_bridge.cpp)

target_include_directories(tessera_animation_loop PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_animation_loop PUBLIC tessera_animation tessera_runtime)

target_compile_features(tessera_animation_loop PUBLIC cxx_std_23)
