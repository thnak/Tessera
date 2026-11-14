# SPDX-License-Identifier: MIT

add_library(tessera_diagnostics STATIC
    crash.cpp
    log.cpp
    memory.cpp
    metrics.cpp
    overlay.cpp)

# Span tracing is compiled out by default (root CMakeLists.txt, TESSERA_ENABLE_TRACING): the source
# is not even listed, so no recorder code, event ring or Chrome JSON encoder can reach a binary that
# did not ask for it. Enabling the option restores the real recorder.
if (TESSERA_ENABLE_TRACING)
    target_sources(tessera_diagnostics PRIVATE trace.cpp)
    target_compile_definitions(tessera_diagnostics PUBLIC TESSERA_ENABLE_TRACING=1)
endif()

target_include_directories(tessera_diagnostics PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_diagnostics PUBLIC cxx_std_23)

if(WIN32)
    target_link_libraries(tessera_diagnostics PRIVATE psapi)
endif()
