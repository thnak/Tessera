// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/schema/project_files.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

// The host half of the project-file policy: walking a real directory and handing what it found to
// `tessera::schema` (the shared judge, which the Wasm module links too). The walk never follows a
// symbolic link, never descends into a pruned directory, and never reads a file whose kind is
// `Other`; a link that leaves the root is a named refusal, not a skipped entry.
namespace tessera::mcp {

namespace policy = tessera::schema;

struct ListedFile {
    std::string path; // project-relative POSIX
    policy::ProjectFileKind kind = policy::ProjectFileKind::Other;
    std::uint64_t size = 0;
    std::string digest; // empty for an unreadable (`Other`) file
};

struct ProjectListing {
    std::vector<ListedFile> files; // sorted by path, byte order
    std::string entry;             // `tessera.json` `entry`, empty when absent
};

// Lists `root`. Fails with a `TESSERA_SEC_` error when a path is unusable, a symbolic link leaves
// the root, or a limit of the shared policy is crossed.
[[nodiscard]] std::expected<ProjectListing, policy::ProjectFileError>
list_project(const std::filesystem::path& root);

// The text of a readable listed file. Callers pass a path that came from `list_project`.
[[nodiscard]] std::expected<std::string, policy::ProjectFileError>
read_project_file(const std::filesystem::path& root, const ListedFile& file);

} // namespace tessera::mcp
