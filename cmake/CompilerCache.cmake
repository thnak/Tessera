# SPDX-License-Identifier: MIT

option(TESSERA_USE_COMPILER_CACHE "Use ccache/sccache when available" ON)

if (TESSERA_USE_COMPILER_CACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(TESSERA_COMPILER_CACHE NAMES sccache ccache)
    if (TESSERA_COMPILER_CACHE)
        set(CMAKE_CXX_COMPILER_LAUNCHER
            "${TESSERA_COMPILER_CACHE}"
            CACHE STRING "C++ compiler launcher" FORCE)
        message(STATUS "Tessera: using compiler cache: ${TESSERA_COMPILER_CACHE}")
    else()
        message(STATUS "Tessera: ccache/sccache not found; building without a compiler cache")
    endif()
endif()
