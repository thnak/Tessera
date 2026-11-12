// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::automation {

// Bounds for untrusted project files (docs/specs/headless-automation-v1.md section 2.1).
inline constexpr std::size_t kMaxAppConfigBytes = std::size_t{256} << 10U; // 256 KiB
inline constexpr std::size_t kMaxManifestBytes = std::size_t{8} << 20U;    // 8 MiB
inline constexpr std::size_t kMaxManifestNodes = 20'000;
inline constexpr std::size_t kMaxManifestComponents = 1'024;
inline constexpr std::size_t kMaxManifestStringBytes = 512;
inline constexpr std::size_t kMaxStyleFiles = 16;
inline constexpr std::size_t kMaxStyleFileBytes = std::size_t{256} << 10U; // 256 KiB each
inline constexpr std::size_t kMaxStyleTotalBytes = std::size_t{1} << 20U;  // 1 MiB overall
inline constexpr int kSupportedManifestVersion = 1;
inline constexpr std::string_view kManifestFileName = "automation.manifest.json";

enum class AppConfigErrorCode {
    ConfigUnreadable,
    ConfigTooLarge,
    ConfigInvalid,
    ManifestMissing,
    ManifestTooLarge,
    ManifestInvalid,
    ManifestUnsupportedVersion,
    ManifestTooManyNodes,
    ManifestDuplicateId,
    StyleInvalid,
    StyleUnreadable,
    StyleTooLarge,
};

[[nodiscard]] std::string_view to_string(AppConfigErrorCode code) noexcept;

struct AppConfigError {
    AppConfigErrorCode code = AppConfigErrorCode::ConfigInvalid;
    std::string message;
};

struct ManifestComponent {
    std::uint64_t component_id = 0;
    std::string name;
    std::string source;
};

// One node of `automation.manifest.json`, in document order. `path` is the structural path
// (`Counter/root[0]/Button[1]`); the parent of a node is the node whose path is this one minus its
// last segment.
struct ManifestNode {
    std::uint64_t ui_id = 0;
    std::uint64_t component_id = 0;
    std::string kind;
    std::string path;
    std::string name; // static aria-label from the compiler; empty when the node has none
    std::string source_file;
    std::int64_t line = 0;
    std::int64_t column = 0;
};

struct AppManifest {
    std::string project_salt;
    std::vector<ManifestComponent> components;
    std::vector<ManifestNode> nodes;
    // `ui.styles` files of tessera.json, concatenated in order with a newline after each; empty
    // when the project lists none. `parse_manifest` never fills it: only the loader knows the
    // config directory.
    std::string stylesheet;
};

// Parses and validates manifest text: version, bounded sizes, decimal non-zero unique ids, unique
// paths. Pure; no file access.
[[nodiscard]] std::expected<AppManifest, AppConfigError> parse_manifest(std::string_view text);

// Reads `config_path` (the script's `app.config`, a relative path to a `tessera.json`), validates
// the fields the harness needs (`schemaVersion` 1, an `app` object, optional `codegen.outDir`),
// then loads `<config dir>/<outDir, default "generated">/automation.manifest.json` and the
// optional `ui.styles` CSS files (safe `.css` paths relative to the config directory).
[[nodiscard]] std::expected<AppManifest, AppConfigError>
load_app_manifest(std::string_view config_path);

} // namespace tessera::automation
