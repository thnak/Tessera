add_library(tessera_stb STATIC
    stb_impl/stb_image_write_impl.cpp)

target_include_directories(tessera_stb PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/stb>)

if (MSVC)
    target_compile_definitions(tessera_stb PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
