# SPDX-License-Identifier: MIT

# Phase 0 proof of concept (ADR-001): Vulkan 1.3 dynamic rendering into an offscreen image, read
# back to host memory, written as a PNG. No window, no swapchain.
#
# This target deliberately does NOT use VMA. ADR-001 point 4 says the Phase 0 PoC "uses raw
# `vkAllocateMemory` only to keep the proof dependency-free", and the proof is the evidence that
# Vulkan 1.3 works on the target hardware with nothing but the loader. It therefore lives here, in
# `tools/poc/`, and not in `src/rendering/`: memory-model-v1 section 4 forbids raw `vkAllocateMemory`
# in the backend, and the rule is only mechanically true if the one exempt program is outside the
# directory the rule names (issue #704).

find_package(Vulkan REQUIRED)

if (Vulkan_GLSLC_EXECUTABLE)
    set(TESSERA_POC_GLSLC "${Vulkan_GLSLC_EXECUTABLE}")
else()
    find_program(TESSERA_POC_GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}/Bin" "${VULKAN_SDK}/Bin")
endif()

if (NOT TESSERA_POC_GLSLC)
    message(FATAL_ERROR "glslc not found. Install the Vulkan SDK and make sure VULKAN_SDK is set.")
endif()

set(TESSERA_POC_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(TESSERA_POC_SHADER_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${TESSERA_POC_SHADER_GEN_DIR})

# The PoC's own shader rule. It cannot reuse the backend's `tessera_add_spv_c` because that function
# is defined in the backend directory's scope and reads its shader sources from there; the PoC owns
# both of its shaders, so it owns the rule. The manifest check still runs first, as for every shader
# rule in the tree (docs/specs/shader-manifest-v1.md section 5).
function(tessera_poc_add_spv_c shader out_var)
    get_filename_component(name ${shader} NAME)
    string(REPLACE "." "_" name ${name})
    set(output ${TESSERA_POC_SHADER_GEN_DIR}/${name}_spv.h)
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${TESSERA_SHADER_MANIFEST_CHECK} ${TESSERA_SHADER_MANIFEST}
        COMMAND ${TESSERA_POC_GLSLC} -mfmt=c --target-env=vulkan1.3
                ${TESSERA_POC_SHADER_DIR}/${shader} -o ${output}
        DEPENDS ${TESSERA_POC_SHADER_DIR}/${shader} ${TESSERA_SHADER_MANIFEST}
                ${TESSERA_SHADER_MANIFEST_CHECK_DEP}
        COMMENT "validate shader manifest; glslc ${shader} -> ${output}"
        VERBATIM)
    set(${out_var} ${output} PARENT_SCOPE)
endfunction()

tessera_poc_add_spv_c(fullscreen.frag TESSERA_POC_FRAG_SPV)

add_executable(vulkan_offscreen_poc
    main.cpp
    offscreen_renderer.cpp
    offscreen_renderer_record.cpp
    ${TESSERA_POC_FRAG_SPV}
    # Naming the backend's generated header as a source is what orders this target after the rule
    # that produces it; the include directory alone would not.
    ${TESSERA_VULKAN_VERT_SPV})

target_include_directories(vulkan_offscreen_poc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${TESSERA_POC_SHADER_GEN_DIR}
    # fullscreen.vert is shared with the glass-blur and storage-probe pipelines, so the backend
    # compiles it once and the PoC includes that generated header rather than duplicating the rule.
    ${TESSERA_VULKAN_SHADER_GEN_DIR})

target_link_libraries(vulkan_offscreen_poc PRIVATE Vulkan::Vulkan tessera_stb)
target_compile_features(vulkan_offscreen_poc PRIVATE cxx_std_23)
