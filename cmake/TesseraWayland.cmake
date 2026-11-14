# SPDX-License-Identifier: MIT
#
# Wayland protocol codegen (issue #155).
#
# A Wayland client needs C code generated from the protocol XML the compositor implements; the
# generated code is a build artifact and is never committed (`wayland-scanner` is the reference
# generator from libwayland, an approved MIT dependency). This is the same build-time-codegen shape
# `tessera_add_spv_c` uses for shaders: one custom command per output, wired as a target source.
#
# Requires TESSERA_WAYLAND_SCANNER (wayland-scanner executable) from the root detection block; a
# project that includes this module without it gets a clear configure error instead of a missing
# header much later.

set(TESSERA_WAYLAND_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated/wayland")

# tessera_add_wayland_client_protocol(<xml> <basename> <header_var> <source_var>)
#
# Runs `wayland-scanner client-header` and `wayland-scanner private-code` on <xml>, producing
# <basename>-client-protocol.h and <basename>-protocol.c under
# ${TESSERA_WAYLAND_GENERATED_DIR}. The two output paths are returned in <header_var>/<source_var>,
# the generated directory is added to the include path, and both files become dependencies of the
# calling target through the returned source path (add <source_var> to target_sources).
function(tessera_add_wayland_client_protocol xml basename header_var source_var)
    if (NOT TESSERA_WAYLAND_SCANNER)
        message(FATAL_ERROR
            "tessera_add_wayland_client_protocol(${basename}) needs wayland-scanner "
            "(TESSERA_WAYLAND_SCANNER is not set; see the root CMakeLists Wayland detection)")
    endif()
    if (NOT EXISTS "${xml}")
        message(FATAL_ERROR
            "tessera_add_wayland_client_protocol(${basename}): protocol XML not found: ${xml}")
    endif()

    file(MAKE_DIRECTORY "${TESSERA_WAYLAND_GENERATED_DIR}")
    set(header "${TESSERA_WAYLAND_GENERATED_DIR}/${basename}-client-protocol.h")
    set(source "${TESSERA_WAYLAND_GENERATED_DIR}/${basename}-protocol.c")

    add_custom_command(
        OUTPUT "${header}"
        COMMAND "${TESSERA_WAYLAND_SCANNER}" client-header "${xml}" "${header}"
        DEPENDS "${xml}"
        COMMENT "wayland-scanner client-header ${basename} -> ${header}"
        VERBATIM)
    add_custom_command(
        OUTPUT "${source}"
        COMMAND "${TESSERA_WAYLAND_SCANNER}" private-code "${xml}" "${source}"
        DEPENDS "${xml}" "${header}"
        COMMENT "wayland-scanner private-code ${basename} -> ${source}"
        VERBATIM)

    set(${header_var} "${header}" PARENT_SCOPE)
    set(${source_var} "${source}" PARENT_SCOPE)
endfunction()
