// SPDX-License-Identifier: MIT
//
// The platform half of the file-system HAL (ADR-020; ADR-050 AF7, issue #179).
//
// `filesystem.cpp` implements everything that is portable; these three functions are the part that
// is not, and each platform TU (`src/platform/windows/filesystem_win32.cpp`,
// `src/platform/linux/filesystem_posix.cpp`) implements them:
//
//   * `platform_replace` - the atomic rename. Windows `MoveFileEx(MOVEFILE_REPLACE_EXISTING)` (and
//     `MOVEFILE_WRITE_THROUGH` for the durable case); POSIX `rename(2)`;
//   * `platform_flush` - `FlushFileBuffers` / `fsync` on the file itself;
//   * `platform_flush_directory` - `fsync` on the directory on POSIX; a successful no-op on
//   Windows,
//     where the exclusive/write-through replace already made the directory entry durable.

#pragma once

#include "tessera/hal/filesystem.hpp"

#include <string>
#include <string_view>

namespace tessera::hal::detail {

// Replace `to` with `from` atomically. Must be a *same-file-system* rename: the caller guarantees
// it by placing `from` in `to`'s directory.
[[nodiscard]] FileResult platform_replace(std::string_view from, std::string_view to);

// Flush the file's data and metadata to stable storage.
[[nodiscard]] FileResult platform_flush(std::string_view path);

// Make the directory's entries durable. Succeeds on platforms where the replace already did.
[[nodiscard]] FileResult platform_flush_directory(std::string_view path);

} // namespace tessera::hal::detail
