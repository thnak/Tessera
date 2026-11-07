# SPDX-License-Identifier: MIT

add_executable(vulkan_offscreen_poc
    main.cpp)

target_link_libraries(vulkan_offscreen_poc PRIVATE tessera_vulkan)
