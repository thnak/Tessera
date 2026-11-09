# SPDX-License-Identifier: MIT

add_executable(tessera_asset_gen
    tessera_asset_gen/main.cpp)

target_include_directories(tessera_asset_gen PRIVATE
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_asset_gen PRIVATE cxx_std_23)
