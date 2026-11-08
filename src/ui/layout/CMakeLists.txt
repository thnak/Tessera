# SPDX-License-Identifier: MIT

add_library(tessera_layout STATIC
    layout_engine.cpp)

target_include_directories(tessera_layout PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_layout PUBLIC
    tessera_style
    tessera_yoga)

target_compile_features(tessera_layout PUBLIC cxx_std_23)
