# SPDX-License-Identifier: MIT
#
# The native-surface HAL seam (ADR-020 rule 1; ADR-050 section 7, roadmap AF6, issue #178).
#
# It is a HAL module because the acceptance for #178 is an ownership boundary, and that boundary is
# only enforceable where the handle type lives: `hal::NativeSurface` is the one type that holds the
# opaque `NativeWindowHandle` pair, and it holds it *borrowed* - a weak witness of the owning
# window's lifetime plus a frozen `SurfaceDesc`. Renderers composite into a surface through it; UI
# code never sees it and keeps reading `WindowMetrics` (ADR-020 rule 1).
#
# No platform library and no backend is linked: every operation in `surface.cpp` is pointer
# bookkeeping, so this module builds identically on a headless lane. The platform half (creating the
# window and lending the handles) already lives in `tessera_hal_window`.
#
# Registered from `src/hal/window/CMakeLists.txt` beside the clipboard module until the root
# CMakeLists (Lead-owned) lists this directory directly; moving the `add_subdirectory` line later is
# a no-op for the target itself.

add_library(tessera_hal_surface STATIC
    surface.cpp)

target_include_directories(tessera_hal_surface PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_hal_surface PUBLIC cxx_std_23)

# The ownership test target is built here (it links this library and nothing else), but its
# `add_test` lives in `tests/gpu/surfaces/CMakeLists.txt` alongside the renderer and golden entries:
# `src/hal/window` - the directory that registers this module - is not part of the CTest subdirectory
# tree, so an `add_test` here would build but never run. `tests/gpu` is in the tree, which is the
# same reason the paint module registers its tests from its own subdirectory.
if (TESSERA_BUILD_TESTS)
    add_executable(native_surface_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/hal/native_surface_tests.cpp)
    target_link_libraries(native_surface_tests PRIVATE tessera_hal_surface)
endif()
