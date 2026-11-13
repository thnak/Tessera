// SPDX-License-Identifier: MIT

#pragma once

#include "project_identity.hpp"

#include <optional>
#include <string>
#include <string_view>

// Package versions of the codegen CLI (docs/specs/project-config-v1.md `packages`, ADR-047 item 1,
// issue #234). A `tessera.json` package entry may name the version the application requires; the
// package root then carries a `tessera-package.json` manifest with its own `name` and `version`,
// and the driver compares the two before anything is compiled.
namespace tessera::compiler::codegen::cli::internal {

// A `MAJOR.MINOR.PATCH` version of the package manifest. Pre-release and build metadata are not
// part of V1: a library ships plain releases.
struct SemVer {
    unsigned major = 0;
    unsigned minor = 0;
    unsigned patch = 0;

    friend bool operator==(const SemVer&, const SemVer&) = default;
};

// What the application asks of a package: the exact version, or `^X.Y.Z` (same major, at least
// X.Y.Z).
struct VersionRequirement {
    SemVer version;
    bool caret = false;
};

[[nodiscard]] std::optional<SemVer> parse_semver(std::string_view text);
[[nodiscard]] std::optional<VersionRequirement> parse_requirement(std::string_view text);

// Whether `installed` satisfies `required`.
[[nodiscard]] bool satisfies(const SemVer& installed, const VersionRequirement& required);

// Checks every package entry that declares a `version` against its `tessera-package.json`. Returns
// true when all pass or none declares one. On a failure it reports one `tessera.json:1:1:
// TESSERA_PACKAGE_*` message, sets exit code 1 and returns false. A package without a `version`
// is never checked, so a plain local package needs no manifest.
[[nodiscard]] bool check_package_versions(const ProjectIdentity& identity, Outcome& outcome);

// The project-relative manifest path of one package root.
[[nodiscard]] std::string manifest_path(std::string_view package_root);

} // namespace tessera::compiler::codegen::cli::internal
