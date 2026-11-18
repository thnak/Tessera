# SPDX-License-Identifier: MIT

# Embeds one generated MSL file as a C++ raw string literal (ADR-053 decision 2).
#
# Run at build time by a custom command in src/rendering/backend/metal/CMakeLists.txt, after the
# cross-compile target that produces MSL_SOURCE. A configure-time read cannot work: the MSL does not
# exist yet when the backend directory is configured on a clean tree.
#
# Invoked as: cmake -DMSL_SOURCE=<file.metal> -DMSL_OUTPUT=<file.inc> -P embed_msl.cmake

if (NOT DEFINED MSL_SOURCE OR NOT DEFINED MSL_OUTPUT)
    message(FATAL_ERROR "embed_msl.cmake needs MSL_SOURCE and MSL_OUTPUT")
endif()

if (NOT EXISTS "${MSL_SOURCE}")
    message(FATAL_ERROR
        "embed_msl: ${MSL_SOURCE} is missing. Configure with -DTESSERA_SHADER_CROSS=ON so the MSL "
        "shader table can be built (ADR-053, docs/guides/writing-a-backend.md section 7.1).")
endif()

file(READ "${MSL_SOURCE}" contents)

# The raw-string delimiter must not occur in the MSL, and C++ limits it to 16 characters (a longer
# one is a compile error, which is how the first attempt failed). A short, unlikely token is used
# and its absence is checked rather than assumed.
set(delimiter "TESSERA_MSL_END")
string(LENGTH "${delimiter}" delimiter_length)
if (delimiter_length GREATER 16)
    message(FATAL_ERROR "embed_msl: the raw-string delimiter must be at most 16 characters.")
endif()
string(FIND "${contents}" ")${delimiter}\"" collision)
if (NOT collision EQUAL -1)
    message(FATAL_ERROR
        "embed_msl: ${MSL_SOURCE} contains the raw-string delimiter; pick another one.")
endif()

file(WRITE "${MSL_OUTPUT}" "R\"${delimiter}(${contents})${delimiter}\"")
