# SPDX-License-Identifier: MIT

add_library(tessera_damage STATIC
    damage.cpp
    redraw_plan.cpp)

target_include_directories(tessera_damage PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_damage PUBLIC tessera_rhi)

target_compile_features(tessera_damage PUBLIC cxx_std_23)
