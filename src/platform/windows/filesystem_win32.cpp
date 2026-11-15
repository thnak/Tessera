// SPDX-License-Identifier: MIT
//
// The Windows half of the file-system HAL (ADR-050 AF7, issue #179).
//
// **Why this file exists.** Windows is the lane where "just call rename" is wrong, twice over:
//
//   * `std::rename` (the C runtime, and what `std::filesystem::rename` bottoms out in on MSVC)
//   fails
//     with EEXIST when the destination already exists. That is exactly the case an atomic replace
//     is for, so it cannot be used.
//   * `std::filesystem::rename` is specified as "rename"; the replace-over-existing behaviour is
//   not
//     part of the standard. Relying on it makes the write work on one lane and fail on the other.
//
// The primitive that *is* specified for this is `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING`, and
// that is what `platform_replace` calls. `MOVEFILE_WRITE_THROUGH` is added for the durable case,
// which is also why `platform_flush_directory` has nothing left to do here: the write-through flag
// flushes the rename itself.
//
// Everything in this file is a blocking Win32 call. It is only ever reached from an
// `IoTaskExecutor` worker (`Storage`'s work body), never from the UI thread (ADR-021).
//
// **Claimed, not assumed.** The semantics that matter - replace-over-existing, and the target
// surviving an aborted write - are asserted by `appfw_storage`'s real-file test
// (`tests/unit/appfw_storage_atomic_tests.cpp`, the `native_file_system` case) rather than trusted
// from this comment.

#include "filesystem_internal.hpp"
#include "tessera/hal/filesystem.hpp"

#include <string>
#include <vector>
#include <windows.h>

namespace tessera::hal::detail {
namespace {

[[nodiscard]] std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                        length);
    return wide;
}

[[nodiscard]] FileError last_error(std::string_view step) {
    return FileError{FileErrorCode::Io, step, static_cast<std::int64_t>(GetLastError())};
}

// Opens `path` for reading and writing without truncating, which is what a flush needs: the file's
// bytes are already written, and only the buffers need pushing to the volume.
[[nodiscard]] bool open_for_flush(const std::wstring& path, HANDLE& handle) {
    handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    return handle != INVALID_HANDLE_VALUE;
}

} // namespace

FileResult platform_replace(std::string_view from, std::string_view to) {
    const std::wstring source = widen(from);
    const std::wstring target = widen(to);
    if (source.empty() || target.empty()) {
        return std::unexpected(FileError{FileErrorCode::Io, "replace", 0});
    }
    // MOVEFILE_REPLACE_EXISTING is the whole point: the destination is replaced atomically.
    // MOVEFILE_WRITE_THROUGH makes the rename itself durable before it returns.
    const DWORD flags = MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH;
    if (MoveFileExW(source.c_str(), target.c_str(), flags) == 0) {
        return std::unexpected(last_error("replace"));
    }
    return {};
}

FileResult platform_flush(std::string_view path) {
    const std::wstring wide = widen(path);
    if (wide.empty()) {
        return std::unexpected(FileError{FileErrorCode::Io, "flush", 0});
    }
    HANDLE handle = INVALID_HANDLE_VALUE;
    if (!open_for_flush(wide, handle)) {
        return std::unexpected(last_error("flush"));
    }
    const BOOL flushed = FlushFileBuffers(handle);
    const DWORD flush_error = flushed == 0 ? GetLastError() : 0;
    CloseHandle(handle);
    if (flushed == 0) {
        return std::unexpected(
            FileError{FileErrorCode::Io, "flush", static_cast<std::int64_t>(flush_error)});
    }
    return {};
}

FileResult platform_flush_directory(std::string_view) {
    // `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)` in `platform_replace` already flushed the
    // directory entry, and Win32 offers no supported way to fsync a directory handle (opening one
    // needs FILE_FLAG_BACKUP_SEMANTICS and FlushFileBuffers on it is not documented to do
    // anything). A successful no-op is the honest answer; reporting Unsupported would make every
    // `Durable` write fail on this platform for no benefit.
    return {};
}

} // namespace tessera::hal::detail
