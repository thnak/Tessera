# SPDX-License-Identifier: MIT

add_library(tessera_software STATIC
    cpu_canvas.cpp)

target_include_directories(tessera_software PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_software PUBLIC tessera_rhi)

target_compile_features(tessera_software PUBLIC cxx_std_23)
