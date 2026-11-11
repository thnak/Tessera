# SPDX-License-Identifier: MIT

add_library(tessera_diagnostics STATIC
    crash.cpp
    log.cpp
    memory.cpp
    metrics.cpp
    overlay.cpp
    trace.cpp)

target_include_directories(tessera_diagnostics PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_diagnostics PUBLIC cxx_std_23)

if(WIN32)
    target_link_libraries(tessera_diagnostics PRIVATE psapi)
endif()
