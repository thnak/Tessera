# SPDX-License-Identifier: MIT

add_library(tessera_hal_window STATIC)

# Private shared implementation header (normalized event ring).
target_include_directories(tessera_hal_window PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})

if (WIN32)
    target_sources(tessera_hal_window PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/windows/window.cpp)
    target_link_libraries(tessera_hal_window PUBLIC user32)
elseif (TESSERA_X11_FOUND)
    target_sources(tessera_hal_window PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/linux/window.cpp)
    target_link_libraries(tessera_hal_window PUBLIC X11::X11)
else()
    target_sources(tessera_hal_window PRIVATE
        window_stub.cpp)
endif()

target_include_directories(tessera_hal_window PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

# The base Window fallback and the public window_wait.hpp helper use the HAL steady clock.
target_link_libraries(tessera_hal_window PUBLIC tessera_hal_timer)

target_compile_features(tessera_hal_window PUBLIC cxx_std_23)
