// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::hal {

// One entry in a directory listing. Only the name is exposed: the storage module composes paths
// itself, and a full `path` type here would drag `std::filesystem` into the public seam.
struct DirEntry {
    std::string name;
    std::uint64_t size = 0;
    bool is_file = false;
};

enum class FileErrorCode : std::uint8_t {
    None = 0,
    NotFound,      // the path does not exist
    NotADirectory, // a path component is not a directory
    AccessDenied,
    Io,          // read/write/flush/rename failed at the OS boundary
    TooLarge,    // a read exceeded the caller's cap
    Unsupported, // the platform cannot provide the requested guarantee
};

struct FileError {
    FileErrorCode code = FileErrorCode::Io;
    // The step that failed ("read", "write", "flush", "replace", ...). Stable strings so tests and
    // diagnostics can assert on them; 0 native value means it does not apply.
    std::string_view step;
    std::int64_t native = 0;

    [[nodiscard]] bool operator==(const FileError&) const = default;
};

using FileResult = std::expected<void, FileError>;
using FileReadResult = std::expected<std::string, FileError>;

// The durability policy a write asks for (docs/design/services/storage.md section 4:
// "fsync policy documented per platform").
//
// Both policies flush the *temp file* before the rename, so a reader never observes a half-written
// document and a crash cannot leave a target whose bytes are still in the page cache while the
// rename is already durable. `Durable` additionally flushes the *directory* after the rename: on
// POSIX the new directory entry is the part that is not on disk yet, so `fsync(dir)` is what makes
// the rename itself survive a power loss. On Windows `MoveFileEx` with `MOVEFILE_WRITE_THROUGH`
// covers the rename and there is nothing further to flush.
//
// `BestEffort` is the default because a directory flush per write is a real cost for a UI-facing
// store, and its failure mode is narrow: a crash may lose the *last* write, but the file is either
// the complete old content or the complete new content, never a mixture.
enum class Durability : std::uint8_t {
    BestEffort = 0,
    Durable = 1,
};

// The OS-specific half of the storage module. Every method is a blocking syscall (or a short
// sequence of them) and is called only from an `IoTaskExecutor` worker, never from the UI thread
// (ADR-021).
//
// A conforming implementation must provide:
//
//   * `replace(from, to)` is **atomic**: afterwards `to` holds the complete old content or the
//     complete new content, is never missing and is never a mixture. `from` and `to` are always in
//     the same directory (the storage module creates its temp file beside the target so a
//     cross-device rename - which is not atomic - cannot happen). On Windows this is
//     `MoveFileEx(MOVEFILE_REPLACE_EXISTING)`, on POSIX `rename(2)`. A plain `std::rename` is *not*
//     a legal implementation: on Windows it fails when the target exists.
//   * `flush(path)` gets the file's bytes to stable storage and *reports* the failure instead of
//     swallowing it (`fsync` / `FlushFileBuffers`).
//   * `flush_directory(path)` makes the directory entry durable (`fsync` on the directory on POSIX;
//     a successful no-op on Windows, where the exclusive replace already covers it).
class FileSystem {
public:
    virtual ~FileSystem() = default;

    FileSystem(const FileSystem&) = delete;
    FileSystem& operator=(const FileSystem&) = delete;

    [[nodiscard]] virtual FileReadResult read(std::string_view path, std::size_t max_bytes) = 0;

    // Creates `path` (and its parent directories) and replaces its content with `data`.
    [[nodiscard]] virtual FileResult write(std::string_view path, std::string_view data) = 0;

    [[nodiscard]] virtual FileResult flush(std::string_view path) = 0;
    [[nodiscard]] virtual FileResult flush_directory(std::string_view path) = 0;

    // Atomic replace: `to` ends up with `from`'s content and `from` no longer exists.
    [[nodiscard]] virtual FileResult replace(std::string_view from, std::string_view to) = 0;

    [[nodiscard]] virtual FileResult remove(std::string_view path) = 0;
    [[nodiscard]] virtual FileResult create_directory(std::string_view path) = 0;
    [[nodiscard]] virtual bool exists(std::string_view path) = 0;
    [[nodiscard]] virtual std::expected<std::uint64_t, FileError> size(std::string_view path) = 0;

    // Sorted by name and deterministic, so a test can compare it directly.
    [[nodiscard]] virtual std::expected<std::vector<DirEntry>, FileError>
    list(std::string_view directory) = 0;

protected:
    FileSystem() = default;
};

// The in-tree implementation for the platform this library was built for. Callers own the returned
// object. (The storage module's tests use their own fake instead, so they never touch a real disk.)
class NativeFileSystem final : public FileSystem {
public:
    NativeFileSystem() = default;

    [[nodiscard]] FileReadResult read(std::string_view path, std::size_t max_bytes) override;
    [[nodiscard]] FileResult write(std::string_view path, std::string_view data) override;
    [[nodiscard]] FileResult flush(std::string_view path) override;
    [[nodiscard]] FileResult flush_directory(std::string_view path) override;
    [[nodiscard]] FileResult replace(std::string_view from, std::string_view to) override;
    [[nodiscard]] FileResult remove(std::string_view path) override;
    [[nodiscard]] FileResult create_directory(std::string_view path) override;
    [[nodiscard]] bool exists(std::string_view path) override;
    [[nodiscard]] std::expected<std::uint64_t, FileError> size(std::string_view path) override;
    [[nodiscard]] std::expected<std::vector<DirEntry>, FileError>
    list(std::string_view directory) override;
};

std::unique_ptr<FileSystem> create_native_file_system();

} // namespace tessera::hal
