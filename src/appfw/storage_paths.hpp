// SPDX-License-Identifier: MIT
//
// Small helpers shared by the storage module's translation units (`Storage` and `CacheStore`):
// path joining, the file-error mapping and the temp-name rule. They are internal so the two stores
// cannot drift on what a temp file is or how a root and a name are joined.

#pragma once

#include "tessera/appfw/storage.hpp"

#include <string>
#include <string_view>

namespace tessera::appfw::detail {

[[nodiscard]] inline std::string join_path(std::string_view root, std::string_view name) {
    if (root.empty()) {
        return std::string(name);
    }
    std::string path(root);
    if (path.back() != '/' && path.back() != '\\') {
        path.push_back('/');
    }
    path.append(name);
    return path;
}

[[nodiscard]] inline StorageError from_file_error(std::string_view step,
                                                  const hal::FileError& error) {
    StorageErrorCode code = StorageErrorCode::Io;
    if (error.code == hal::FileErrorCode::NotFound) {
        code = StorageErrorCode::NotFound;
    } else if (error.code == hal::FileErrorCode::TooLarge) {
        code = StorageErrorCode::TooLarge;
    } else if (error.code == hal::FileErrorCode::Unsupported) {
        code = StorageErrorCode::Unsupported;
    }
    return StorageError{code, step, error.native};
}

// The suffix `atomic_replace` gives a temp file. A listing filters these out, so a crashed process
// cannot make a half-written file look like an entry.
[[nodiscard]] inline bool is_temp_name(std::string_view name) noexcept {
    return name.size() > 4 && name.substr(name.size() - 4) == ".tmp";
}

} // namespace tessera::appfw::detail
