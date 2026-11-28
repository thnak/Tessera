# SPDX-License-Identifier: MIT

# The streaming dashboard sample (issue #184): a renderer-agnostic scene + scripted load. The CPU
# paint goldens and the allocation proof in tests/gpu link it; the CLI drives it for frame-budget
# numbers on a real machine. It needs no GPU, so it builds on every lane.
add_library(tessera_stream_dashboard STATIC stream_dashboard.cpp)
target_include_directories(tessera_stream_dashboard PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tessera_stream_dashboard PUBLIC tessera_paint tessera_damage)
target_compile_features(tessera_stream_dashboard PUBLIC cxx_std_23)

add_executable(stream_dashboard main.cpp)
target_include_directories(stream_dashboard PRIVATE ${PROJECT_SOURCE_DIR}/tools/poc/batched2d)
target_link_libraries(stream_dashboard PRIVATE tessera_stream_dashboard tessera_software
    tessera_diagnostics tessera_stb)
