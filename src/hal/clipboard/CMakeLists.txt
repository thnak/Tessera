# SPDX-License-Identifier: MIT

add_library(tessera_hal_clipboard STATIC
    clipboard.cpp)

if (WIN32)
    target_sources(tessera_hal_clipboard PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/windows/clipboard_win32.cpp)
    target_link_libraries(tessera_hal_clipboard PUBLIC user32)
else()
    target_sources(tessera_hal_clipboard PRIVATE
        clipboard_unsupported.cpp)
endif()

target_include_directories(tessera_hal_clipboard PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_hal_clipboard PUBLIC cxx_std_23)
