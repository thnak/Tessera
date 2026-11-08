# SPDX-License-Identifier: MIT

find_package(Vulkan REQUIRED)

set(TESSERA_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(TESSERA_SHADER_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${TESSERA_SHADER_GEN_DIR})

if (Vulkan_GLSLC_EXECUTABLE)
    set(TESSERA_GLSLC "${Vulkan_GLSLC_EXECUTABLE}")
else()
    find_program(TESSERA_GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}/Bin" "${VULKAN_SDK}/Bin")
endif()

if (NOT TESSERA_GLSLC)
    message(FATAL_ERROR
        "glslc not found. Install the Vulkan SDK and make sure VULKAN_SDK is set.")
endif()

function(tessera_add_spv_c shader out_var)
    get_filename_component(name ${shader} NAME)
    string(REPLACE "." "_" name ${name})
    set(output ${TESSERA_SHADER_GEN_DIR}/${name}_spv.h)
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${TESSERA_GLSLC} -mfmt=c --target-env=vulkan1.3
                ${TESSERA_SHADER_DIR}/${shader} -o ${output}
        DEPENDS ${TESSERA_SHADER_DIR}/${shader}
        COMMENT "glslc ${shader} -> ${output}"
        VERBATIM)
    set(${out_var} ${output} PARENT_SCOPE)
endfunction()

tessera_add_spv_c(fullscreen.vert TESSERA_VERT_SPV)
tessera_add_spv_c(fullscreen.frag TESSERA_FRAG_SPV)
tessera_add_spv_c(batched2d.vert TESSERA_BATCHED2D_VERT_SPV)
tessera_add_spv_c(batched2d.frag TESSERA_BATCHED2D_FRAG_SPV)

add_custom_target(tessera_vulkan_shaders DEPENDS ${TESSERA_VERT_SPV} ${TESSERA_FRAG_SPV})
add_custom_target(tessera_batched2d_shaders
    DEPENDS ${TESSERA_BATCHED2D_VERT_SPV} ${TESSERA_BATCHED2D_FRAG_SPV})

add_library(tessera_vulkan STATIC
    src/offscreen_renderer.cpp
    ${TESSERA_VERT_SPV}
    ${TESSERA_FRAG_SPV})

target_include_directories(tessera_vulkan PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_include_directories(tessera_vulkan PRIVATE
    ${TESSERA_SHADER_GEN_DIR})
target_link_libraries(tessera_vulkan PUBLIC Vulkan::Vulkan tessera_stb)
target_compile_features(tessera_vulkan PUBLIC cxx_std_23)
add_dependencies(tessera_vulkan tessera_vulkan_shaders)

add_library(tessera_rhi_vulkan STATIC
    src/vulkan_device.cpp
    src/vulkan_instance.cpp
    src/vulkan_resources.cpp
    src/vulkan_command_list.cpp
    src/vulkan_texture.cpp
    src/vulkan_pipeline.cpp
    src/vma_impl.cpp
    src/batched2d_pipeline.cpp
    ${TESSERA_BATCHED2D_VERT_SPV}
    ${TESSERA_BATCHED2D_FRAG_SPV}
    ${PROJECT_SOURCE_DIR}/third_party/volk/volk.c)

target_include_directories(tessera_rhi_vulkan PUBLIC
    ${PROJECT_SOURCE_DIR}/include)
target_include_directories(tessera_rhi_vulkan PRIVATE
    ${TESSERA_SHADER_GEN_DIR}
    ${PROJECT_SOURCE_DIR}/third_party/volk
    ${PROJECT_SOURCE_DIR}/third_party/vma)

target_compile_definitions(tessera_rhi_vulkan PRIVATE VK_NO_PROTOTYPES)
target_link_libraries(tessera_rhi_vulkan PUBLIC tessera_rhi Vulkan::Vulkan)
target_compile_features(tessera_rhi_vulkan PUBLIC cxx_std_23)
add_dependencies(tessera_rhi_vulkan tessera_batched2d_shaders)

if (MSVC)
    set_source_files_properties(src/vma_impl.cpp PROPERTIES COMPILE_OPTIONS "/W0")
    set_source_files_properties(
        ${PROJECT_SOURCE_DIR}/third_party/volk/volk.c PROPERTIES COMPILE_OPTIONS "/W0")
else()
    set_source_files_properties(src/vma_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
    set_source_files_properties(
        ${PROJECT_SOURCE_DIR}/third_party/volk/volk.c PROPERTIES COMPILE_OPTIONS "-w")
endif()
