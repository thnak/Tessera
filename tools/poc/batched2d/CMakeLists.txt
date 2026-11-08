# SPDX-License-Identifier: MIT

add_executable(batched2d_poc
    main.cpp)

target_link_libraries(batched2d_poc PRIVATE
    tessera_gpu
    tessera_rhi_vulkan
    tessera_rhi_null
    tessera_stb)

target_compile_features(batched2d_poc PRIVATE cxx_std_23)
