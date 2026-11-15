# SPDX-License-Identifier: MIT
#
# The file-system HAL seam (ADR-020; ADR-050 AF7, issue #179). It owns the two primitives an atomic
# write cannot portably fake in C++:
#
#   * `replace(from, to)` - an *atomic* replace of an existing target. On Windows this is
#     `MoveFileEx(..., MOVEFILE_REPLACE_EXISTING)`; the C runtime `rename()` fails when the target
#     exists, and `std::filesystem::rename` is not specified to replace at all, so neither is used.
#     On POSIX it is `rename(2)`, which is atomic within a file system and replaces atomically.
#   * `flush(path)` / `flush_directory(path)` - `FlushFileBuffers` on Windows, `fsync` on POSIX,
#     including the directory flush that makes the new directory entry durable.
#
# Everything else (read/write/remove/list) is portable enough to be the same code on both lanes, but
# it lives here too because these are the calls the "never block the UI thread" rule is about: they
# are only ever legal from an `IoTaskExecutor` worker.

add_library(tessera_hal_filesystem STATIC
    filesystem.cpp)

target_include_directories(tessera_hal_filesystem PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

# The private header that names the three platform primitives (replace/flush/flush-directory). The
# platform TUs live in src/platform/{windows,linux}, so they need this directory on the include path.
target_include_directories(tessera_hal_filesystem PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(tessera_hal_filesystem PUBLIC cxx_std_23)

if (WIN32)
    target_sources(tessera_hal_filesystem PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/windows/filesystem_win32.cpp)
else()
    target_sources(tessera_hal_filesystem PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/linux/filesystem_posix.cpp)
endif()
