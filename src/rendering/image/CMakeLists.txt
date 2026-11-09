# SPDX-License-Identifier: MIT

add_library(tessera_image STATIC
    image_decode_cache.cpp
    image_loader.cpp
    image_texture_cache.cpp)

target_include_directories(tessera_image PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_image PUBLIC cxx_std_23)

# stb_image is vendored and already built as tessera_stb; this makes the dependency production-linked
# (decode of embedded image assets), so its license/notice/SBOM rows are updated with this slice.
target_link_libraries(tessera_image PUBLIC
    tessera_rhi
    tessera_stb)

# Image tests are registered from this subdirectory so the lane does not edit the shared
# tests/unit registry while other lanes are working in it.
if (TESSERA_BUILD_TESTS)
    enable_testing()

    set(TESSERA_IMAGE_FIXTURE_DIR "${PROJECT_SOURCE_DIR}/tests/fixtures/assets")

    add_executable(image_loader_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/image_loader_tests.cpp)
    target_link_libraries(image_loader_tests PRIVATE tessera_image)
    target_compile_definitions(image_loader_tests PRIVATE
        TESSERA_IMAGE_FIXTURE_DIR="${TESSERA_IMAGE_FIXTURE_DIR}")
    add_test(NAME image_loader COMMAND image_loader_tests)

    add_executable(image_texture_cache_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/image_texture_cache_tests.cpp)
    target_include_directories(image_texture_cache_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/unit)
    target_link_libraries(image_texture_cache_tests PRIVATE tessera_image)
    add_test(NAME image_texture_cache COMMAND image_texture_cache_tests)
endif()