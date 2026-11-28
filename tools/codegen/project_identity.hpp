// SPDX-License-Identifier: MIT

#pragma once

#include "regenerate.hpp"
#include "tessera/compiler/jsx/limits.hpp"

#include <cstdint>
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
    // The version the application requires (`X.Y.Z` or `^X.Y.Z`, see package_manifest.hpp); empty
    // means no check.
    std::string version;
};

struct ThemePinSpec {
    std::string theme;
    std::string pin; // MAJOR, MAJOR.MINOR or MAJOR.MINOR.PATCH
};

// `dependencies` of the project config (project-config-v1.md): what the generated CMake target
// adds. `modules` names the opt-in framework modules the app links ("sqlite" is
// `tessera_appfw_storage_sqlite`, issue #983); the build switches the matching CMake option.
struct DependenciesSpec {
    std::vector<std::string> include_dirs; // project-relative POSIX directories
    std::vector<std::string> link_libraries;
    std::vector<std::string> modules; // closed set, deduplicated, document order
};

// The project identity of one run: the id salt (empty: ids stay unassigned) and the root that
// source paths are relative to (docs/specs/automation-ids-v1.md section 2).
struct ProjectIdentity {
    std::string salt;
    std::filesystem::path root;
    std::vector<PackageRootSpec> packages;
    DependenciesSpec dependencies;
    // `assets.theme` as a project-relative POSIX path, empty when the config names no theme asset.
    std::string theme;
    // `assets.themePins`: theme id -> version pin, in document order (ADR-047 item 4). Checked
    // against the theme asset's `themeVersions` when the asset is embedded.
    std::vector<ThemePinSpec> theme_pins;
    // `assets.fontPack` as a project-relative POSIX path to a `font-pack.json`, empty when the app
    // does not opt into the bundled pack. When set, the theme asset's `fontPacks` pins are checked
    // against the manifest's version when the asset is embedded (theme-versions-v1.md section 5).
    std::string font_pack;
    // `ui.limits.*`: the JSX front end limits, the compiled-in defaults unless the config sets one.
    tessera::compiler::jsx::Limits limits;
    // `permissions`: the mask of the permissions this app may request (docs/specs/permissions-v1.md
    // section 7). Empty by default, so an app that declares nothing can request nothing; the
    // codegen driver turns a request for an undeclared permission into a `TESSERA_PERM_UNDECLARED`
    // diagnostic.
    std::uint64_t permissions = 0;
};

// Appends one message; an error also makes the exit code 1 unless it was already larger. The
// three-argument form is for a message with no source diagnostic behind it (setup, usage, codegen);
// its record keeps an empty code and no span rather than inventing either
// (docs/specs/compiler-diagnostics-v1.md section 2).
void report(Outcome& outcome, bool is_error, std::string text);
void report(Outcome& outcome, bool is_error, std::string text, DiagnosticRecord record);

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
