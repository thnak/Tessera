# SPDX-License-Identifier: MIT

add_library(tessera_style STATIC
    cascade.cpp
    color.cpp
    compiled_stylesheet.cpp
    keyframes.cpp
    media_query.cpp
    parser.cpp
    property_registry.cpp
    scene_style.cpp
    supports.cpp
    selector.cpp
    selector_match.cpp
    tokenizer.cpp)

target_include_directories(tessera_style PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_style PUBLIC tessera_scene)

target_compile_features(tessera_style PUBLIC cxx_std_23)
