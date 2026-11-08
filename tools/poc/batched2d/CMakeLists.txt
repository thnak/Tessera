# SPDX-License-Identifier: MIT

add_executable(batched2d_poc
    main.cpp)

target_link_libraries(batched2d_poc PRIVATE
    tessera_gpu
    tessera_hal_window
    tessera_runtime
    tessera_rhi_vulkan
    tessera_rhi_null
    tessera_text_render
    tessera_diagnostics
    tessera_golden
    tessera_stb)

target_compile_features(batched2d_poc PRIVATE cxx_std_23)
