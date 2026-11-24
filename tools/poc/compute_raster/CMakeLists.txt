# SPDX-License-Identifier: MIT

# ADR-071 spike (issue #683): a GPU compute path rasterizer, fill only, measured at 4K against the
# ThorVG CPU raster. Off by default (TESSERA_BUILD_COMPUTE_RASTER_SPIKE); it changes no default and
# is not registered with ctest. The shaders are compiled by this rule alone, so they are outside the
# shader manifest (they are evidence, not production shaders).

if (NOT Vulkan_GLSLC_EXECUTABLE)
    message(FATAL_ERROR "compute_raster spike needs glslc (Vulkan SDK)")
endif()

set(SPIKE_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${SPIKE_GEN})
set(SPIKE_HEADERS)
foreach (pass IN ITEMS CLEAR BIN FINE)
    string(TOLOWER ${pass} lower)
    set(header ${SPIKE_GEN}/raster_${lower}_spv.h)
    add_custom_command(
        OUTPUT ${header}
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} -mfmt=c --target-env=vulkan1.3 -DPASS_${pass}=1
                ${CMAKE_CURRENT_SOURCE_DIR}/raster.comp -o ${header}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/raster.comp
        COMMENT "glslc raster.comp (${pass})"
        VERBATIM)
    list(APPEND SPIKE_HEADERS ${header})
endforeach()

add_executable(compute_raster_spike main.cpp ${SPIKE_HEADERS})
target_include_directories(compute_raster_spike PRIVATE ${SPIKE_GEN})
target_link_libraries(compute_raster_spike PRIVATE tessera_rhi_vulkan tessera_path)
target_compile_features(compute_raster_spike PRIVATE cxx_std_23)
target_compile_definitions(compute_raster_spike PRIVATE _CRT_SECURE_NO_WARNINGS)
