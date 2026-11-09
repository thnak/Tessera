# SPDX-License-Identifier: MIT

add_library(tessera_path STATIC
    path_rasterizer.cpp
    path_texture_cache.cpp
    path_texture_key.cpp)

target_include_directories(tessera_path PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_path PUBLIC cxx_std_23)

target_link_libraries(tessera_path PUBLIC tessera_rhi)

if (TESSERA_ENABLE_THORVG)
    target_sources(tessera_path PRIVATE
        thorvg_path_rasterizer.cpp)
    target_compile_definitions(tessera_path PRIVATE TESSERA_HAVE_THORVG=1)
    target_link_libraries(tessera_path PRIVATE tessera_thorvg)
endif()

# Layering gate (configure time): the third-party vector rasterizer must not leak out of its
# adapter. Only src/rendering/path/thorvg_path_rasterizer.cpp may include a ThorVG header; public
# headers, UI code, renderer code and the PoC tools go through the PathRasterizer seam. The check
# runs in every configuration, including the default one where ThorVG is disabled.
set(_path_adapter_name "thorvg_path_rasterizer.cpp")
set(_path_layering_violations "")
file(GLOB_RECURSE _path_scan_files
    "${PROJECT_SOURCE_DIR}/include/tessera/*.hpp"
    "${PROJECT_SOURCE_DIR}/src/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/*.hpp"
    "${PROJECT_SOURCE_DIR}/tools/*.cpp"
    "${PROJECT_SOURCE_DIR}/tools/*.hpp")
foreach(_path_file IN LISTS _path_scan_files)
    get_filename_component(_path_file_name "${_path_file}" NAME)
    if (_path_file_name STREQUAL _path_adapter_name)
        continue()
    endif()
    file(READ "${_path_file}" _path_file_content)
    if (_path_file_content MATCHES "#[ \t]*include[ \t]*[<\"]thorvg[./]")
        list(APPEND _path_layering_violations "${_path_file}")
    endif()
endforeach()
if (_path_layering_violations)
    string(REPLACE ";" "\n  " _path_violation_text "${_path_layering_violations}")
    message(FATAL_ERROR
        "ThorVG headers may only be included by src/rendering/path/${_path_adapter_name}; "
        "found in:\n  ${_path_violation_text}")
endif()

# Path-owned cache test. It is registered from this subdirectory (source still under tests/unit)
# so the path lane never edits the shared tests/unit registry while other lanes are working in it.
# The subdirectory is added before the root enable_testing() call, so enable it here; CTest still
# reads the top-level CTestTestfile. The GPU draw test lives in tests/gpu because it needs the
# batched2d renderer.
if (TESSERA_BUILD_TESTS)
    enable_testing()

    add_executable(path_texture_cache_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/path_texture_cache_tests.cpp)
    target_link_libraries(path_texture_cache_tests PRIVATE tessera_path)
    add_test(NAME path_texture_cache COMMAND path_texture_cache_tests)
endif()
