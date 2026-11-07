# SPDX-License-Identifier: MIT

add_library(tessera_rhi_null STATIC
    null_device.cpp)

target_include_directories(tessera_rhi_null PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_rhi_null PUBLIC tessera_rhi)

target_compile_features(tessera_rhi_null PUBLIC cxx_std_20)
