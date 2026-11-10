# SPDX-License-Identifier: MIT

# Headless automation harness: the JSON-lines protocol codec and its command/result model
# (`docs/specs/headless-automation-v1.md`). The slice owns its targets here so parallel slices
# never edit the root subdirectory list at the same time.
#
# Leaf library: strict JSON-lines framing (UTF-8, bounded line length, CRLF tolerance, EOF), the
# line transport seam with an in-process implementation, the versioned request/response envelope,
# and the snapshot/action/trace/audit command model. It links nothing from the project so the
# harness binary, tests, and any in-process embedding can share the same bytes.
add_library(tessera_harness STATIC
    in_process_transport.cpp
    json_parse.cpp
    json_string.cpp
    json_value.cpp
    json_write.cpp
    line_framing.cpp
    protocol.cpp
    utf8.cpp)

target_include_directories(tessera_harness PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_harness PUBLIC cxx_std_23)
