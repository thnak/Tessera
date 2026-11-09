# SPDX-License-Identifier: MIT
#
# Deterministic build flags (reproducible binaries).
#
# Included from cmake/CompilerCache.cmake (which the root CMakeLists.txt already includes before any
# target is defined), so the flags apply uniformly to Tessera and third-party targets without
# changing the shared root CMakeLists.txt.
#
# Goal: two clean builds of the same commit with the same toolchain in different build directories
# produce a byte-identical executable. Debug information is kept; only absolute source/build paths
# and the linker timestamp embedded in the binary are suppressed.
#
# - MSVC: /Brepro makes the linker emit a deterministic PE timestamp (and a deterministic debug
#   directory entry), and the linker's /PATHMAP rewrites absolute source/build paths recorded in
#   PDB debug information to relative ones (it is a link.exe option, not a cl option).
# - GCC/Clang: -ffile-prefix-map rewrites paths in __FILE__/macros and -fdebug-prefix-map rewrites
#   paths in debug information; both source and build roots are mapped so a different build
#   directory cannot leak in.
#
# Not applied here: linker build-ids (they are content-derived and deterministic), and stripping
# debug information (explicitly out of scope; paths are made deterministic instead).

if(MSVC)
    add_link_options(/Brepro)
    add_link_options("/PATHMAP:${CMAKE_SOURCE_DIR}=.")
    add_link_options("/PATHMAP:${CMAKE_BINARY_DIR}=.")
else()
    add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")
    add_compile_options("-ffile-prefix-map=${CMAKE_BINARY_DIR}=.")
    add_compile_options("-fdebug-prefix-map=${CMAKE_SOURCE_DIR}=.")
    add_compile_options("-fdebug-prefix-map=${CMAKE_BINARY_DIR}=.")
endif()
