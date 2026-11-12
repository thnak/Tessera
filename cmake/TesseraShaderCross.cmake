# SPDX-License-Identifier: MIT

# Opt-in build-time shader cross-compilation (ADR-049, TESSERA_SHADER_CROSS, default OFF).
#
# The authored GLSL is compiled a second time with TESSERA_SHADER_PORTABLE defined, which swaps the
# Vulkan push-constant block and combined image sampler for the interface every other API shares
# (std140 uniform at binding 0, texture at 1, sampler at 2). That portable SPIR-V is then
# translated, entirely inside the build directory, by:
#   - SPIRV-Cross (Vulkan SDK) -> MSL and HLSL, with the clip-space Y flip those APIs need;
#   - spirv-opt -O then naga   -> WGSL (naga flips Y by default; spirv-opt removes OpCopyLogical,
#                                 which naga's SPIR-V front end rejects).
# Nothing is committed and nothing runs at application run time. A missing tool skips its outputs
# with a status message; only glslc is mandatory. When tests are on, ctest validates every input
# and output a tool can check: spirv-val, naga (generated and authored WGSL), dxc (HLSL).

# One pin for CMake and scripts/ci/validate_wgsl.py.
file(STRINGS ${PROJECT_SOURCE_DIR}/tools/naga-version.txt TESSERA_NAGA_VERSION_PIN LIMIT_COUNT 1)

find_program(TESSERA_CROSS_GLSLC NAMES glslc
    HINTS "${Vulkan_GLSLC_EXECUTABLE}" "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
find_program(TESSERA_SPIRV_VAL NAMES spirv-val HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
find_program(TESSERA_SPIRV_OPT NAMES spirv-opt HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
find_program(TESSERA_SPIRV_CROSS NAMES spirv-cross
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
find_program(TESSERA_DXC NAMES dxc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
find_program(TESSERA_NAGA NAMES naga
    HINTS "$ENV{CARGO_HOME}/bin" "$ENV{USERPROFILE}/.cargo/bin" "$ENV{HOME}/.cargo/bin")

if (NOT TESSERA_CROSS_GLSLC)
    message(FATAL_ERROR "TESSERA_SHADER_CROSS=ON needs glslc (Vulkan SDK); set VULKAN_SDK.")
endif()

if (TESSERA_NAGA)
    execute_process(COMMAND "${TESSERA_NAGA}" --version
        OUTPUT_VARIABLE tessera_naga_version OUTPUT_STRIP_TRAILING_WHITESPACE)
    if (NOT tessera_naga_version STREQUAL TESSERA_NAGA_VERSION_PIN)
        message(WARNING "naga ${tessera_naga_version} found, pinned ${TESSERA_NAGA_VERSION_PIN}; "
            "WGSL output may differ (cargo install naga-cli --version ${TESSERA_NAGA_VERSION_PIN})")
    endif()
endif()

foreach (tool SPIRV_VAL SPIRV_OPT SPIRV_CROSS DXC NAGA)
    if (TESSERA_${tool})
        message(STATUS "Shader cross: ${tool} = ${TESSERA_${tool}}")
    else()
        message(STATUS "Shader cross: ${tool} not found; its outputs and checks are skipped")
    endif()
endforeach()

# tessera_cross_compile_shader(<shader_id> SOURCE_DIR <dir> VERTEX <file> FRAGMENT <file>
#                              [AUTHORED_WGSL <file>])
# Writes <build>/shaders/cross/<shader_id>/<shader_id>.<vert|frag>.{portable.spv,metal,hlsl,wgsl}
# and adds the target tessera_shader_cross_<shader_id> to ALL.
function(tessera_cross_compile_shader shader_id)
    cmake_parse_arguments(ARG "" "SOURCE_DIR;VERTEX;FRAGMENT;AUTHORED_WGSL" "" ${ARGN})
    set(out_dir ${CMAKE_BINARY_DIR}/shaders/cross/${shader_id})
    file(MAKE_DIRECTORY ${out_dir})
    set(outputs)

    foreach (stage vert frag)
        if (stage STREQUAL "vert")
            set(source ${ARG_SOURCE_DIR}/${ARG_VERTEX})
            set(dxc_profile vs_6_0)
        else()
            set(source ${ARG_SOURCE_DIR}/${ARG_FRAGMENT})
            set(dxc_profile ps_6_0)
        endif()
        set(base ${out_dir}/${shader_id}.${stage})

        add_custom_command(OUTPUT ${base}.portable.spv
            COMMAND tessera_shader_manifest_check ${TESSERA_SHADER_MANIFEST}
            COMMAND ${TESSERA_CROSS_GLSLC} --target-env=vulkan1.3 -DTESSERA_SHADER_PORTABLE=1
                    ${source} -o ${base}.portable.spv
            DEPENDS ${source} ${TESSERA_SHADER_MANIFEST}
            COMMENT "validate shader manifest; glslc (portable) ${shader_id}.${stage}"
            VERBATIM)
        list(APPEND outputs ${base}.portable.spv)
        if (TESSERA_SPIRV_VAL AND TESSERA_BUILD_TESTS)
            add_test(NAME shader_cross_${shader_id}_${stage}_spirv_val
                COMMAND ${TESSERA_SPIRV_VAL} --target-env vulkan1.3 ${base}.portable.spv)
        endif()

        if (TESSERA_SPIRV_CROSS)
            add_custom_command(OUTPUT ${base}.metal ${base}.hlsl
                COMMAND ${TESSERA_SPIRV_CROSS} ${base}.portable.spv --msl --msl-version 20100
                        --flip-vert-y --output ${base}.metal
                COMMAND ${TESSERA_SPIRV_CROSS} ${base}.portable.spv --hlsl --shader-model 60
                        --flip-vert-y --output ${base}.hlsl
                DEPENDS ${base}.portable.spv
                COMMENT "spirv-cross ${shader_id}.${stage} -> MSL, HLSL"
                VERBATIM)
            list(APPEND outputs ${base}.metal ${base}.hlsl)
            if (TESSERA_DXC AND TESSERA_BUILD_TESTS)
                add_test(NAME shader_cross_${shader_id}_${stage}_hlsl_dxc
                    COMMAND ${TESSERA_DXC} -T ${dxc_profile} -E main ${base}.hlsl
                            -Fo ${base}.dxil)
            endif()
        endif()

        if (TESSERA_NAGA AND TESSERA_SPIRV_OPT)
            add_custom_command(OUTPUT ${base}.wgsl
                COMMAND ${TESSERA_SPIRV_OPT} -O ${base}.portable.spv -o ${base}.opt.spv
                COMMAND ${TESSERA_NAGA} ${base}.opt.spv ${base}.wgsl
                DEPENDS ${base}.portable.spv
                COMMENT "naga ${shader_id}.${stage} -> WGSL"
                VERBATIM)
            list(APPEND outputs ${base}.wgsl)
            if (TESSERA_BUILD_TESTS)
                add_test(NAME shader_cross_${shader_id}_${stage}_wgsl_naga
                    COMMAND ${TESSERA_NAGA} ${base}.wgsl)
            endif()
        endif()
    endforeach()

    if (ARG_AUTHORED_WGSL AND TESSERA_NAGA AND TESSERA_BUILD_TESTS)
        add_test(NAME shader_cross_${shader_id}_authored_wgsl_naga
            COMMAND ${TESSERA_NAGA} ${ARG_AUTHORED_WGSL})
    endif()

    add_custom_target(tessera_shader_cross_${shader_id} ALL DEPENDS ${outputs})
endfunction()
