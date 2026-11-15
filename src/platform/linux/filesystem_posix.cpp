// SPDX-License-Identifier: MIT
//
// The POSIX (Linux) half of the file-system HAL (ADR-050 AF7, issue #179).
//
// `rename(2)` is the primitive here: within one file system it is atomic and it replaces an
// existing destination, which is exactly the contract `FileSystem::replace` documents. It is *not*
// atomic across file systems (the kernel falls back to a copy), which is why the storage module
// puts its temp file in the target's own directory.
//
// `platform_flush` is `fsync(2)` on the file. `platform_flush_directory` opens the directory
// `O_RDONLY | O_DIRECTORY` and fsyncs it, which is what makes the rename's new directory entry
// durable; without it a power loss can leave the file's bytes on disk under the *old* name.
//
// Everything here is blocking and only ever runs on an `IoTaskExecutor` worker (ADR-021).

#include "filesystem_internal.hpp"
#include "tessera/hal/filesystem.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace tessera::hal::detail {
namespace {

[[nodiscard]] FileError errno_error(std::string_view step) {
    return FileError{FileErrorCode::Io, step, static_cast<std::int64_t>(errno)};
}

} // namespace

FileResult platform_replace(std::string_view from, std::string_view to) {
    if (from.empty() || to.empty()) {
        return std::unexpected(FileError{FileErrorCode::Io, "replace", 0});
    }
    const std::string source(from);
    const std::string target(to);
    if (::rename(source.c_str(), target.c_str()) != 0) {
        return std::unexpected(errno_error("replace"));
    }
    return {};
}

FileResult platform_flush(std::string_view path) {
    const std::string target(path);
    const int descriptor = ::open(target.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return std::unexpected(errno_error("flush"));
    }
    const int flushed = ::fsync(descriptor);
    const int saved = errno;
    ::close(descriptor);
    if (flushed != 0) {
        return std::unexpected(
            FileError{FileErrorCode::Io, "flush", static_cast<std::int64_t>(saved)});
    }
    return {};
}

FileResult platform_flush_directory(std::string_view path) {
    const std::string target(path.empty() ? std::string(".") : std::string(path));
    const int descriptor = ::open(target.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        return std::unexpected(errno_error("flush-dir"));
    }
    const int flushed = ::fsync(descriptor);
    const int saved = errno;
    ::close(descriptor);
    if (flushed != 0) {
        // EINVAL is what a file system that does not support directory fsync reports; treating it
        // as a failure would make `Durable` unusable there, so it is reported as Unsupported.
        if (saved == EINVAL) {
            return std::unexpected(FileError{FileErrorCode::Unsupported, "flush-dir", saved});
        }
        return std::unexpected(
            FileError{FileErrorCode::Io, "flush-dir", static_cast<std::int64_t>(saved)});
    }
    return {};
}

} // namespace tessera::hal::detail
