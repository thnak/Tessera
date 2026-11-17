# SPDX-License-Identifier: MIT

# CSS modules (issue #463, slice 2b): the scan that hashes a module file's project-relative path and
# rewrites its class selectors and keyframes to scoped names. It links only the scene-free tokenizer
# (`tessera_css_tokenizer`), never `tessera_style`, so the compiler can scan a `.module.css` file
# without dragging the scene graph, the cascade or the layout stack into the compiler.
add_library(tessera_css_module STATIC
    module_hash.cpp
    module_rewrite.cpp
    module_scan.cpp
    module_selector.cpp
    module_walk.cpp)

target_include_directories(tessera_css_module
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tessera_css_module PUBLIC tessera_css_tokenizer)

target_compile_features(tessera_css_module PUBLIC cxx_std_23)

# Registered here, not in the shared tests/unit registry, so a lane adding tests never edits a file
# another lane is working in (the same precedent as src/ui/style/CMakeLists.txt).
if (TESSERA_BUILD_TESTS)
    enable_testing()

    add_executable(css_module_scan_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/css_module_scan_tests.cpp)
    target_link_libraries(css_module_scan_tests PRIVATE tessera_css_module)
    add_test(NAME css_module_scan COMMAND css_module_scan_tests)
endif ()
