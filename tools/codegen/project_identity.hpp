// SPDX-License-Identifier: MIT

#pragma once

#include "regenerate.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Project identity and import resolution of the codegen CLI, split out of the driver so each
// translation unit stays reviewable. The driver owns parsing, lowering and output; this unit owns
// `tessera.json` (salt, root and package roots) and the specifier -> project file mapping.
namespace tessera::compiler::codegen::cli::internal {

// One package root of the project config (docs/specs/project-config-v1.md `packages`, issue #70): a
// bare specifier resolves inside it, so a TSX component library is compiled by the same pipeline.
struct PackageRootSpec {
    std::string name;
    std::string root; // project-relative POSIX directory
};

// The project identity of one run: the id salt (empty: ids stay unassigned) and the root that
// source paths are relative to (docs/specs/automation-ids-v1.md section 2).
struct ProjectIdentity {
    std::string salt;
    std::filesystem::path root;
    std::vector<PackageRootSpec> packages;
    // `assets.theme` as a project-relative POSIX path, empty when the config names no theme asset.
    std::string theme;
};

// Appends one message; an error also makes the exit code 1 unless it was already larger.
void report(Outcome& outcome, bool is_error, std::string text);

std::optional<std::string> read_file(const std::filesystem::path& path);

// `--salt`, else `app.namespace` of the `tessera.json` at `--root` or found by walking up from the
// first input; a config that is found but has no usable namespace is an error, not a silent skip.
std::optional<ProjectIdentity> resolve_identity(const Options& options, Outcome& outcome);

// The project-relative file an import names, with the `.tsx` extension the front end reads.
// Relative specifiers resolve against the importing file, bare ones against the package roots, and
// `"tessera"`/`"react"` never name a file. Nullopt means "not a project file"; lowering reports a
// specifier that does not resolve.
std::optional<std::string> import_target(std::string_view from_file, std::string_view specifier,
                                         const ProjectIdentity& identity);

} // namespace tessera::compiler::codegen::cli::internal
