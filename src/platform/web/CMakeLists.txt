# SPDX-License-Identifier: MIT

# The web HAL (ADR-046 item 3, issue #290). Two halves on purpose:
#
#   tessera_web_hal      pure C++ (DOM event translation, the rAF scheduling policy, WebHost). It has no
#                        browser or Emscripten dependency, so it builds and is tested on every lane.
#   tessera_web_exports  the extern "C" surface and JS imports; Emscripten only.
add_library(tessera_web_hal STATIC
    web_frame_scheduler.cpp
    web_host.cpp
    web_input_translation.cpp
    web_surface_extent.cpp)

# The headers are private to the web HAL (the app target reaches them through this library), like
# the other src/platform headers; tests add the directory by linking the target.
target_include_directories(tessera_web_hal PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(tessera_web_hal PUBLIC tessera_input tessera_runtime)
target_compile_features(tessera_web_hal PUBLIC cxx_std_23)

if (EMSCRIPTEN)
    add_library(tessera_web_exports STATIC web_exports.cpp)
    target_link_libraries(tessera_web_exports PUBLIC tessera_web_hal)

    # The web app driver's GPU half (issue #1013): the preinitialized WebGPU device wrapped as an
    # rhi::Device and the canvas surface that hands out one render target per frame. Apps that draw
    # link it; web_smoke does not, so its download budget is unchanged.
    add_library(tessera_web_gpu STATIC web_canvas_surface.cpp)
    target_link_libraries(tessera_web_gpu PUBLIC tessera_web_hal tessera_webgpu)

    # The host page and glue that ship beside every web app (static folder, ADR-046 item 10).
    set(TESSERA_WEB_HOST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/host CACHE INTERNAL "Web host page sources")

    # tessera_web_app(<target>): turns an executable target into the static web app folder:
    # `app.js` + `app.wasm` (MODULARIZE, single-threaded so any static host works without COOP/COEP,
    # ADR-046 item 6) next to index.html and tessera_web_host.js. The target brings its own main()
    # and registers a WebHost through tessera_hal::web::attach_global_host.
    function (tessera_web_app target)
        target_link_libraries(${target} PRIVATE tessera_web_exports)
        set_target_properties(${target} PROPERTIES OUTPUT_NAME app SUFFIX ".js")
        target_link_options(${target} PRIVATE
            "SHELL:-s MODULARIZE=1"
            "SHELL:-s EXPORT_ES6=1"
            "SHELL:-s EXPORT_NAME=createTesseraApp"
            "SHELL:-s ENVIRONMENT=web"
            "SHELL:-s EXPORTED_RUNTIME_METHODS=ccall"
            "SHELL:-s EXPORTED_FUNCTIONS=_main,_tessera_web_pointer,_tessera_web_wheel,_tessera_web_key,_tessera_web_focus,_tessera_web_resize,_tessera_web_visibility,_tessera_web_wake,_tessera_web_frame"
            "--js-library=${TESSERA_WEB_HOST_DIR}/library_tessera_web.js")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy ${TESSERA_WEB_HOST_DIR}/index.html
                    ${TESSERA_WEB_HOST_DIR}/tessera_web_host.js $<TARGET_FILE_DIR:${target}>
            VERBATIM)
    endfunction ()
endif ()

if (TESSERA_BUILD_TESTS AND NOT EMSCRIPTEN)
    enable_testing()

    foreach (_web_test web_input_translation web_frame_scheduler web_host web_surface_extent)
        add_executable(${_web_test}_tests ${PROJECT_SOURCE_DIR}/tests/unit/${_web_test}_tests.cpp)
        target_link_libraries(${_web_test}_tests PRIVATE tessera_web_hal)
        add_test(NAME ${_web_test} COMMAND ${_web_test}_tests)
    endforeach ()

    # The page glue under Node with fake DOM objects (no browser). Skipped, not failed, where Node is
    # absent; CI images carry it for the studio build.
    find_program(TESSERA_NODE node)
    if (TESSERA_NODE)
        add_test(NAME web_host_glue
            COMMAND ${TESSERA_NODE} ${PROJECT_SOURCE_DIR}/tests/web/host_glue_tests.mjs)
    endif ()
endif ()
