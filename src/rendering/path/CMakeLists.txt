# SPDX-License-Identifier: MIT

add_library(tessera_path STATIC
    path_rasterizer.cpp)

target_include_directories(tessera_path PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_path PUBLIC cxx_std_23)

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
