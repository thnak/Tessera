// SPDX-License-Identifier: MIT
//
// The atomic-replace sequence (ADR-050 AF7, issue #179). This is the whole point of the module, so
// it is one small file with one job: given a target path and the complete new bytes, either the
// target ends up with those bytes or it keeps exactly what it had.
//
// Sequence, in order:
//
//   1. make sure the target's parent directory exists
//   2. write the *complete* payload to `<target>.<generation>.tmp` in the target's own directory
//   3. flush the temp file                    <- the bytes are on disk before anything is visible
//   4. atomically replace the target          <- the only step a reader can observe
//   5. `Durable` only: flush the directory    <- the new directory entry is durable
//
// Steps 2 and 3 never touch the target, and step 4 is the only step that makes anything visible.
// Cancellation is checked between whole steps: a token that fires after step 2 removes the temp
// file and leaves the target alone, which is why there is no "partially written target" state to
// recover from.

#include "tessera/appfw/storage.hpp"

#include <string>

namespace tessera::appfw {
namespace {

// Everything before the last separator plus the separator's position; an entry path never crosses
// more than the root, so this is the whole path computation the module needs.
[[nodiscard]] std::string parent_of(std::string_view path) {
    const std::size_t separator = path.find_last_of("/\\");
    if (separator == std::string_view::npos) {
        return {};
    }
    return std::string(path.substr(0, separator));
}

[[nodiscard]] StorageError io_error(std::string_view step, const hal::FileError& error) {
    return StorageError{StorageErrorCode::Io, step, error.native};
}

} // namespace

std::string temp_path_for(std::string_view target, std::uint64_t generation) {
    std::string path(target);
    path.push_back('.');
    // Fixed-width hex: deterministic, sortable, and no character a shell or a filesystem treats
    // specially (the target itself may be any legal path).
    constexpr char kHex[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
        path.push_back(kHex[(generation >> static_cast<unsigned>(shift)) & 0xFu]);
    }
    path += ".tmp";
    return path;
}

bool is_valid_entry_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 128) {
        return false;
    }
    if (name == "." || name == "..") {
        return false;
    }
    // No separator of either platform's convention, no drive prefix, no absolute path: the sandbox
    // rule of docs/design/services/storage.md section 4 ("no absolute/traversal paths from
    // untrusted input").
    for (const char character : name) {
        if (character == '/' || character == '\\' || character == ':') {
            return false;
        }
    }
    for (std::size_t index = 0; index + 1 < name.size(); ++index) {
        if (name[index] == '.' && name[index + 1] == '.') {
            return false;
        }
    }
    return true;
}

std::expected<void, StorageError> atomic_replace(hal::FileSystem& files, std::string_view target,
                                                 std::string_view data, hal::Durability durability,
                                                 const runtime::CancellationToken& token,
                                                 std::uint64_t generation,
                                                 AtomicWriteTrace* trace) {
    if (target.empty()) {
        return std::unexpected(StorageError{StorageErrorCode::InvalidName, "validate"});
    }
    if (token.cancelled()) {
        return std::unexpected(StorageError{StorageErrorCode::Cancelled, "cancelled"});
    }

    const std::string parent = parent_of(target);
    if (!parent.empty()) {
        if (auto created = files.create_directory(parent); !created) {
            return std::unexpected(io_error("create-directory", created.error()));
        }
    }

    const std::string temp = temp_path_for(target, generation);
    if (auto written = files.write(temp, data); !written) {
        // A failed write can still have created a partial temp file, so cleanup runs on this path
        // too: "no temp file behind" covers failures, not only cancellation.
        if (files.exists(temp) && files.remove(temp)) {
            if (trace != nullptr) {
                trace->temp_removed = true;
            }
        }
        return std::unexpected(io_error("write-temp", written.error()));
    }
    if (trace != nullptr) {
        trace->temp_written = true;
    }

    if (auto flushed = files.flush(temp); !flushed) {
        if (files.exists(temp) && files.remove(temp)) {
            if (trace != nullptr) {
                trace->temp_removed = true;
            }
        }
        return std::unexpected(io_error("flush-temp", flushed.error()));
    }
    if (trace != nullptr) {
        trace->temp_flushed = true;
    }

    // The cancellation point that matters: the temp file is complete and the target is untouched,
    // so abandoning here is exactly the "no partial write" case.
    if (token.cancelled()) {
        if (files.exists(temp) && files.remove(temp)) {
            if (trace != nullptr) {
                trace->temp_removed = true;
            }
        }
        return std::unexpected(StorageError{StorageErrorCode::Cancelled, "cancelled"});
    }

    if (auto replaced = files.replace(temp, target); !replaced) {
        if (files.exists(temp) && files.remove(temp)) {
            if (trace != nullptr) {
                trace->temp_removed = true;
            }
        }
        return std::unexpected(io_error("replace", replaced.error()));
    }
    if (trace != nullptr) {
        trace->replaced = true;
    }

    if (durability == hal::Durability::Durable) {
        const std::string_view directory =
            parent.empty() ? std::string_view(".") : std::string_view(parent);
        if (auto flushed = files.flush_directory(directory); !flushed) {
            // The rename already happened, so the new content is visible; only its durability is
            // unconfirmed. Reporting the failure is correct: the caller asked for `Durable`.
            return std::unexpected(io_error("flush-dir", flushed.error()));
        }
        if (trace != nullptr) {
            trace->directory_flushed = true;
        }
    }
    return {};
}

} // namespace tessera::appfw
