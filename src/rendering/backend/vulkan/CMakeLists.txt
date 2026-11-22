# SPDX-License-Identifier: MIT

find_package(Vulkan REQUIRED)

# D7 test seam: exposes fault-injection entry points on the internal VulkanDevice (device loss and
# recovery failure) so the loss path is testable without a real GPU reset. Tests link this target,
# so the definition propagates to them.
option(TESSERA_VULKAN_TEST_HOOKS
    "Expose Vulkan fault-injection seams for device-loss tests" ${TESSERA_BUILD_TESTS})

set(TESSERA_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(TESSERA_SHADER_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${TESSERA_SHADER_GEN_DIR})

# Exported for the Phase 0 offscreen PoC (tools/poc/vulkan_offscreen), which includes the
# `fullscreen_vert_spv.h` generated here: fullscreen.vert is shared with the glass-blur and
# storage-probe pipelines, so it is compiled once, in the backend, and the PoC reuses the header
# rather than duplicating the rule (issue #704).
set(TESSERA_VULKAN_SHADER_GEN_DIR ${TESSERA_SHADER_GEN_DIR} PARENT_SCOPE)

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
    # docs/specs/shader-manifest-v1.md section 5: validate the manifest first in the same rule, so
    # a bad manifest stops this output before glslc starts. This covers both the
    # Vulkan_GLSLC_EXECUTABLE and find_program cases because the check is independent of glslc.
    # The manifest is a dependency so editing it reruns glslc and the check, not just the check.
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${TESSERA_SHADER_MANIFEST_CHECK} ${TESSERA_SHADER_MANIFEST}
        COMMAND ${TESSERA_GLSLC} -mfmt=c --target-env=vulkan1.3
                ${TESSERA_SHADER_DIR}/${shader} -o ${output}
        DEPENDS ${TESSERA_SHADER_DIR}/${shader} ${TESSERA_SHADER_MANIFEST} ${TESSERA_SHADER_MANIFEST_CHECK_DEP}
        COMMENT "validate shader manifest; glslc ${shader} -> ${output}"
        VERBATIM)
    set(${out_var} ${output} PARENT_SCOPE)
endfunction()

# fullscreen.vert is shared: the glass-blur and storage-probe pipelines use it, and so does the
# Phase 0 offscreen PoC (which compiles it into its own generated header). fullscreen.frag is
# PoC-only and lives with the PoC, so this backend directory holds no shader that only a tool uses.
tessera_add_spv_c(fullscreen.vert TESSERA_VERT_SPV)
# Exported so the Phase 0 offscreen PoC can name the generated header as a source, which is what
# gives it a build-order dependency on the rule above (issue #704).
set(TESSERA_VULKAN_VERT_SPV ${TESSERA_VERT_SPV} PARENT_SCOPE)
tessera_add_spv_c(batched2d.vert TESSERA_BATCHED2D_VERT_SPV)
tessera_add_spv_c(batched2d.frag TESSERA_BATCHED2D_FRAG_SPV)
# LCD subpixel variant of the batched 2D fragment program (issue #471 slice 1,
# docs/design/display-aware-rendering.md section 3.4): the same program plus the second fragment
# output a dual-source coverage blend reads. Compiled here so glslc and the manifest check validate
# it in every Vulkan build; no pipeline creates it yet (slice 3 wires it up), and the manifest entry
# deliberately has no WebGPU source because WGSL has no output index.
tessera_add_spv_c(batched2d_lcd.frag TESSERA_BATCHED2D_LCD_FRAG_SPV)

# The LCD variant's vertex stage: the same source compiled with TESSERA_NO_TEXTURE_ARRAY, so it does
# not write the texture-array index its fragment stage does not declare (RHI 1.7, issue #480 part 2).
set(TESSERA_BATCHED2D_LCD_VERT_SPV
    ${TESSERA_SHADER_GEN_DIR}/batched2d_lcd_vert_spv.h)
add_custom_command(
    OUTPUT ${TESSERA_BATCHED2D_LCD_VERT_SPV}
    COMMAND ${TESSERA_SHADER_MANIFEST_CHECK} ${TESSERA_SHADER_MANIFEST}
    COMMAND ${TESSERA_GLSLC} -mfmt=c --target-env=vulkan1.3 -DTESSERA_NO_TEXTURE_ARRAY=1
            ${TESSERA_SHADER_DIR}/batched2d.vert -o ${TESSERA_BATCHED2D_LCD_VERT_SPV}
    DEPENDS ${TESSERA_SHADER_DIR}/batched2d.vert ${TESSERA_SHADER_MANIFEST} ${TESSERA_SHADER_MANIFEST_CHECK_DEP}
    COMMENT "validate shader manifest; glslc batched2d.vert (LCD variant) -> ${TESSERA_BATCHED2D_LCD_VERT_SPV}"
    VERBATIM)

# Descriptor-indexing variant of the batched2d fragment program (RHI 1.7, issue #480 part 2): the
# same source compiled with TESSERA_TEXTURE_ARRAY, so its clip/SDF/shadow logic cannot drift from the
# default variant - only the sampling line differs. The manifest check runs first, as for every rule.
set(TESSERA_BATCHED2D_ARRAY_FRAG_SPV
    ${TESSERA_SHADER_GEN_DIR}/batched2d_array_frag_spv.h)
add_custom_command(
    OUTPUT ${TESSERA_BATCHED2D_ARRAY_FRAG_SPV}
    COMMAND ${TESSERA_SHADER_MANIFEST_CHECK} ${TESSERA_SHADER_MANIFEST}
    COMMAND ${TESSERA_GLSLC} -mfmt=c --target-env=vulkan1.3 -DTESSERA_TEXTURE_ARRAY=1
            ${TESSERA_SHADER_DIR}/batched2d.frag -o ${TESSERA_BATCHED2D_ARRAY_FRAG_SPV}
    DEPENDS ${TESSERA_SHADER_DIR}/batched2d.frag ${TESSERA_SHADER_MANIFEST} ${TESSERA_SHADER_MANIFEST_CHECK_DEP}
    COMMENT "validate shader manifest; glslc batched2d.frag (texture array) -> ${TESSERA_BATCHED2D_ARRAY_FRAG_SPV}"
    VERBATIM)
# Glass two-pass Gaussian (issue #244, docs/specs/glass-v1.md section 5): the fragment pass pairs
# with fullscreen.vert (gl_VertexIndex triangle, no vertex buffer). Compiled here so glslc and the
# manifest check validate it in every Vulkan build, and embedded by glass_blur_pipeline_desc() -
# the pipeline the sampled-target composite test (issue #254) records.
tessera_add_spv_c(glass_blur.frag TESSERA_GLASS_BLUR_FRAG_SPV)

# Storage-buffer conformance probe (RHI 1.12, ADR-068 slice 1): a fragment stage that reads one
# read-only storage buffer and writes what it found, so the conformance suite can prove
# bind_storage_buffer reaches the shader on a real device. Pairs with fullscreen.vert.
tessera_add_spv_c(storage_probe.frag TESSERA_STORAGE_PROBE_FRAG_SPV)

add_custom_target(tessera_batched2d_shaders
    DEPENDS ${TESSERA_BATCHED2D_VERT_SPV} ${TESSERA_BATCHED2D_FRAG_SPV}
            ${TESSERA_BATCHED2D_LCD_FRAG_SPV} ${TESSERA_BATCHED2D_LCD_VERT_SPV}
            ${TESSERA_BATCHED2D_ARRAY_FRAG_SPV}
            ${TESSERA_GLASS_BLUR_FRAG_SPV}
            ${TESSERA_STORAGE_PROBE_FRAG_SPV})

set(TESSERA_VULKAN_RHI_SOURCES
    src/vulkan_device.cpp
    src/vulkan_device_info.cpp
    src/vulkan_device_recovery.cpp
    src/vulkan_descriptor_pool.cpp
    src/vulkan_instance.cpp
    src/vulkan_timestamp_probe.cpp
    src/vulkan_resources.cpp
    src/vulkan_command_list.cpp
    src/vulkan_command_list_copy.cpp
    src/vulkan_sampled_target.cpp
    src/vulkan_command_resources.cpp
    src/vulkan_upload.cpp
    src/vulkan_texture.cpp
    src/vulkan_texture_array.cpp
    src/vulkan_storage_buffer.cpp
    src/vulkan_mipmaps.cpp
    src/vulkan_pipeline.cpp
    src/vulkan_validation.cpp
    src/vma_impl.cpp
    src/batched2d_pipeline.cpp
    src/glass_blur_pipeline.cpp
    src/storage_probe_pipeline.cpp
    ${TESSERA_BATCHED2D_VERT_SPV}
    ${TESSERA_BATCHED2D_FRAG_SPV}
    ${TESSERA_BATCHED2D_LCD_FRAG_SPV}
    ${TESSERA_BATCHED2D_LCD_VERT_SPV}
    ${TESSERA_BATCHED2D_ARRAY_FRAG_SPV}
    ${TESSERA_VERT_SPV}
    ${TESSERA_GLASS_BLUR_FRAG_SPV}
    ${TESSERA_STORAGE_PROBE_FRAG_SPV}
    ${PROJECT_SOURCE_DIR}/third_party/volk/volk.c)

# The presentation-family resolution is part of the RHI target unconditionally: `vulkan_instance.cpp`
# selects its queue family by calling `resolve_present_platform()` / `probe_present_support()` /
# `has_present_support()`, and `PresentPlatform::NoPlatform` is a valid answer on a headless host.
# Gating this TU behind a *window* platform made those symbols vanish on a Linux host with neither
# X11 nor Wayland, which is a link error rather than a degraded build (found by the canvas lane,
# which is the first Linux configuration that builds this backend).
list(APPEND TESSERA_VULKAN_RHI_SOURCES
    src/vulkan_present_platform.cpp)

if (WIN32 OR ANDROID OR TESSERA_X11_FOUND OR TESSERA_WAYLAND_FOUND)
    # The presentation surface itself, plus the bounded acquire/present state machine. These need a
    # real window system; the family resolution above does not.
    list(APPEND TESSERA_VULKAN_RHI_SOURCES
        src/vulkan_swapchain.cpp
        src/vulkan_swapchain_surface.cpp
        src/vulkan_swapchain_present.cpp)
endif()

add_library(tessera_rhi_vulkan STATIC ${TESSERA_VULKAN_RHI_SOURCES})

target_include_directories(tessera_rhi_vulkan PUBLIC
    ${PROJECT_SOURCE_DIR}/include)
target_include_directories(tessera_rhi_vulkan PRIVATE
    ${TESSERA_SHADER_GEN_DIR}
    ${PROJECT_SOURCE_DIR}/third_party/volk
    ${PROJECT_SOURCE_DIR}/third_party/vma)

target_compile_definitions(tessera_rhi_vulkan PRIVATE VK_NO_PROTOTYPES)
if (TESSERA_VULKAN_TEST_HOOKS)
    target_compile_definitions(tessera_rhi_vulkan PUBLIC TESSERA_VULKAN_TEST_HOOKS)
endif()
if (WIN32)
    # vkCreateWin32SurfaceKHR / vkGetPhysicalDeviceWin32PresentationSupportKHR.
    target_compile_definitions(tessera_rhi_vulkan PRIVATE VK_USE_PLATFORM_WIN32_KHR)
    target_link_libraries(tessera_rhi_vulkan PRIVATE user32)
elseif (ANDROID)
    # vkCreateAndroidSurfaceKHR (VK_KHR_android_surface). There is no presentation-support query for
    # this family, and no extra library: libvulkan.so is already the Vulkan::Vulkan target, and the
    # ANativeWindow* comes from the HAL window's native handle.
    target_compile_definitions(tessera_rhi_vulkan PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
else()
    # A Linux build may carry both WSI families: both macros are defined only when both dev packages
    # are truly usable, so the instance can enable both surface extensions (an unused one is
    # harmless) while the resolved family alone is used for presentation (issue #155).
    if (TESSERA_X11_FOUND)
        # vkCreateXlibSurfaceKHR / vkGetPhysicalDeviceXlibPresentationSupportKHR.
        target_compile_definitions(tessera_rhi_vulkan PRIVATE VK_USE_PLATFORM_XLIB_KHR)
        target_link_libraries(tessera_rhi_vulkan PRIVATE X11::X11)
    endif()
    if (TESSERA_WAYLAND_FOUND)
        # vkCreateWaylandSurfaceKHR / vkGetPhysicalDeviceWaylandPresentationSupportKHR.
        target_compile_definitions(tessera_rhi_vulkan PRIVATE VK_USE_PLATFORM_WAYLAND_KHR)
        target_include_directories(tessera_rhi_vulkan PRIVATE
            ${TESSERA_WAYLAND_CLIENT_INCLUDE_DIRS})
        target_link_libraries(tessera_rhi_vulkan PRIVATE ${TESSERA_WAYLAND_CLIENT_LIBRARIES})
        target_link_directories(tessera_rhi_vulkan PRIVATE ${TESSERA_WAYLAND_CLIENT_LIBRARY_DIRS})
    endif()
endif()
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
