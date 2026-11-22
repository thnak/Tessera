// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// The project-file policy of the designer and of the agent surface
// (docs/specs/designer-studio-v1.md sections 7 and 10): which paths a project may name, which files
// count as UI code, and how large a project may be. It is a pure function over names and sizes,
// with no filesystem and no clock, so the native `tessera-mcp` server and the Wasm module link the
// same code and can never disagree about which files count. Walking a directory (or a zip, or a
// File System Access handle) belongs to the host; this unit judges what the host found.
namespace tessera::schema {

// The kind a listed file gets. `Other` is "not accepted UI code": it is shown, never read.
enum class ProjectFileKind : std::uint8_t {
    Source, // `.tsx` / `.jsx`
    Style,  // `.css`
    Config, // `tessera.json`
    Theme,  // the theme asset named by `assets.theme`
    Asset,  // a font or image listed in `tessera.json`
    Other,
};

[[nodiscard]] std::string_view to_string(ProjectFileKind kind) noexcept;

// True for the kinds a tool may read. `Other` is the only unreadable kind.
[[nodiscard]] constexpr bool is_readable(ProjectFileKind kind) noexcept {
    return kind != ProjectFileKind::Other;
}

// The limits of designer-studio-v1.md section 7 (the per-file limit of jsx-subset-v1.md section 15
// and the file-count and total caps of the MCP follow-up). They bound accepted files only: a file
// that is never read costs nothing to refuse, but it still counts toward the file count so a folder
// cannot grow without bound.
inline constexpr std::size_t kMaxProjectFiles = 500;
inline constexpr std::uint64_t kMaxProjectFileBytes = 1024ull * 1024ull;
inline constexpr std::uint64_t kMaxProjectBytes = 20ull * 1024ull * 1024ull;

// A named refusal. `code` is a `TESSERA_SEC_` code (error-model-v1.md section 3); `message` names
// the offending path or the limit, and never an absolute host path.
struct ProjectFileError {
    std::string code;
    std::string message;
};

inline constexpr std::string_view kPathEscapeCode = "TESSERA_SEC_PATH_ESCAPE";
inline constexpr std::string_view kFileLimitCode = "TESSERA_SEC_FILE_LIMIT";

// Judges one project-relative path as the host spelled it. A path must be relative, use `/`, name
// no `.` or `..` segment and no empty segment, hold no NUL or backslash, and fit in 1024 bytes. The
// check is lexical on purpose: a symlink that leaves the root is the host's to detect, because only
// the host can resolve it.
[[nodiscard]] std::optional<ProjectFileError> check_project_path(std::string_view path);

// What `tessera.json` says about the files around it. All paths are project-relative POSIX.
struct ProjectFileContext {
    std::string theme;                   // `assets.theme`, empty when absent
    std::span<const std::string> assets; // fonts and images (a directory entry ends in `/`)
};

// Classifies a path that already passed `check_project_path`. A path under a directory whose name
// is pruned (`is_pruned_directory`) is `Other` whatever its extension.
[[nodiscard]] ProjectFileKind classify_project_file(std::string_view path,
                                                    const ProjectFileContext& context);

// Directories a host does not descend into: `node_modules` and any name starting with `.`. They are
// reported as one `Other` entry instead of being walked, so a dependency tree cannot exhaust the
// file budget or be read.
[[nodiscard]] bool is_pruned_directory(std::string_view name) noexcept;

struct ProjectFileEntry {
    std::string path;
    std::uint64_t size = 0;
    ProjectFileKind kind = ProjectFileKind::Other;
};

// Checks a whole listing against the limits: the file count over every entry, the per-file cap and
// the total over the readable ones. The refusal names the limit and the first path that crossed it.
[[nodiscard]] std::optional<ProjectFileError>
check_project_limits(std::span<const ProjectFileEntry> entries);

// FNV-1a 64 of `bytes` as `fnv1a64:<16 hex digits>`, the digest form of `ir::StylesheetRef::hash`
// (ir-v1.md), so a file digest and an IR stylesheet hash are the same kind of value.
[[nodiscard]] std::string project_file_digest(std::string_view bytes);

} // namespace tessera::schema
