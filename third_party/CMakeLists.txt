#SPDX - License - Identifier : MIT

add_library(tessera_stb STATIC
    stb_impl/stb_image_write_impl.cpp)

target_include_directories(tessera_stb PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/stb>)

if (MSVC)
    target_compile_definitions(tessera_stb PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

file(GLOB_RECURSE YOGA_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/yoga/yoga/*.cpp)

add_library(tessera_yoga STATIC ${YOGA_SOURCES})

target_include_directories(tessera_yoga PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/yoga)

target_compile_features(tessera_yoga PUBLIC cxx_std_20)
target_compile_definitions(tessera_yoga PRIVATE
    $<$<CONFIG:Debug>:DEBUG>)

if (MSVC)
    target_compile_options(tessera_yoga PRIVATE /W0)
else()
    target_compile_options(tessera_yoga PRIVATE -w)
endif()
