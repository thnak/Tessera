# SPDX-License-Identifier: MIT

add_library(tessera_style STATIC
    cascade.cpp
    color.cpp
    color_names.cpp
    compiled_stylesheet.cpp
    container_query.cpp
    keyframes.cpp
    media_query.cpp
    parser.cpp
    prepared_stylesheet.cpp
    stylesheet_blob_read.cpp
    stylesheet_blob_write.cpp
    property_registry.cpp
    scene_style.cpp
    supports.cpp
    selector.cpp
    selector_match.cpp
    tokenizer.cpp
    transform.cpp
    var_resolve.cpp)

target_include_directories(tessera_style PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_style PUBLIC tessera_scene)

target_compile_features(tessera_style PUBLIC cxx_std_23)
