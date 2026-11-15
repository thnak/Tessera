// SPDX-License-Identifier: MIT
//
// The portable half of the file-system HAL (ADR-020; ADR-050 AF7, issue #179).
//
// Read, write, remove and list are the same code on both lanes; the atomic replace and the two
// flush operations are the platform half (`filesystem_internal.hpp`). Keeping the portable code in
// one file means the *step order* the storage module relies on cannot drift between lanes.

#include "filesystem_internal.hpp"
#include "tessera/hal/filesystem.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

namespace tessera::hal {
namespace {

// One place that turns an errno/HRESULT into the seam's value error. The native number is kept for
// the diagnostic; it is never shown to a user verbatim.
[[nodiscard]] FileError error_from(std::string_view step, const std::error_code& code) {
    FileErrorCode mapped = FileErrorCode::Io;
    if (code == std::errc::no_such_file_or_directory) {
        mapped = FileErrorCode::NotFound;
    } else if (code == std::errc::permission_denied) {
        mapped = FileErrorCode::AccessDenied;
    } else if (code == std::errc::not_a_directory) {
        mapped = FileErrorCode::NotADirectory;
    }
    return FileError{mapped, step, static_cast<std::int64_t>(code.value())};
}

} // namespace

FileReadResult NativeFileSystem::read(std::string_view path, std::size_t max_bytes) {
    std::error_code code;
    const std::filesystem::path target{std::string(path)};
    const auto size = std::filesystem::file_size(target, code);
    if (code) {
        return std::unexpected(error_from("read", code));
    }
    if (size > max_bytes) {
        return std::unexpected(FileError{FileErrorCode::TooLarge, "read", 0});
    }

    std::ifstream stream(target, std::ios::binary);
    if (!stream) {
        return std::unexpected(FileError{FileErrorCode::Io, "read", 0});
    }
    std::string bytes;
    bytes.resize(static_cast<std::size_t>(size));
    if (size != 0) {
        stream.read(bytes.data(), static_cast<std::streamsize>(size));
        if (!stream) {
            return std::unexpected(FileError{FileErrorCode::Io, "read", 0});
        }
    }
    return bytes;
}

FileResult NativeFileSystem::write(std::string_view path, std::string_view data) {
    std::error_code code;
    const std::filesystem::path target{std::string(path)};
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), code);
        if (code && code != std::errc::file_exists) {
            return std::unexpected(error_from("write", code));
        }
    }

    std::ofstream stream(target, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return std::unexpected(FileError{FileErrorCode::Io, "write", 0});
    }
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    stream.flush();
    if (!stream) {
        return std::unexpected(FileError{FileErrorCode::Io, "write", 0});
    }
    return {};
}

FileResult NativeFileSystem::flush(std::string_view path) {
    return detail::platform_flush(path);
}

FileResult NativeFileSystem::flush_directory(std::string_view path) {
    return detail::platform_flush_directory(path);
}

FileResult NativeFileSystem::replace(std::string_view from, std::string_view to) {
    return detail::platform_replace(from, to);
}

FileResult NativeFileSystem::remove(std::string_view path) {
    std::error_code code;
    const bool removed = std::filesystem::remove(std::filesystem::path{std::string(path)}, code);
    if (code) {
        return std::unexpected(error_from("remove", code));
    }
    if (!removed) {
        return std::unexpected(FileError{FileErrorCode::NotFound, "remove", 0});
    }
    return {};
}

FileResult NativeFileSystem::create_directory(std::string_view path) {
    if (path.empty()) {
        return {};
    }
    std::error_code code;
    std::filesystem::create_directories(std::filesystem::path{std::string(path)}, code);
    if (code && code != std::errc::file_exists) {
        return std::unexpected(error_from("create-directory", code));
    }
    return {};
}

bool NativeFileSystem::exists(std::string_view path) {
    std::error_code code;
    return std::filesystem::exists(std::filesystem::path{std::string(path)}, code) && !code;
}

std::expected<std::uint64_t, FileError> NativeFileSystem::size(std::string_view path) {
    std::error_code code;
    const auto value = std::filesystem::file_size(std::filesystem::path{std::string(path)}, code);
    if (code) {
        return std::unexpected(error_from("size", code));
    }
    return static_cast<std::uint64_t>(value);
}

std::expected<std::vector<DirEntry>, FileError> NativeFileSystem::list(std::string_view directory) {
    std::error_code code;
    std::vector<DirEntry> entries;
    std::filesystem::directory_iterator iterator{std::filesystem::path{std::string(directory)},
                                                 code};
    if (code) {
        return std::unexpected(error_from("list", code));
    }
    for (const std::filesystem::directory_iterator end; iterator != end; iterator.increment(code)) {
        if (code) {
            return std::unexpected(error_from("list", code));
        }
        const std::filesystem::directory_entry& entry = *iterator;
        DirEntry item;
        item.name = entry.path().filename().string();
        std::error_code status_code;
        item.is_file = entry.is_regular_file(status_code) && !status_code;
        if (item.is_file) {
            item.size = entry.file_size(status_code);
            if (status_code) {
                item.size = 0;
                status_code.clear();
            }
        }
        entries.push_back(std::move(item));
    }
    // Deterministic order, so a caller (and a test) never depends on the OS's enumeration order.
    std::sort(entries.begin(), entries.end(),
              [](const DirEntry& left, const DirEntry& right) { return left.name < right.name; });
    return entries;
}

std::unique_ptr<FileSystem> create_native_file_system() {
    return std::make_unique<NativeFileSystem>();
}

} // namespace tessera::hal
