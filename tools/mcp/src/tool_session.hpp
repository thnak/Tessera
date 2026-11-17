// SPDX-License-Identifier: MIT

#pragma once

#include "preview_tools.hpp"
#include "tessera/hal/fold_posture.hpp"
#include "tool_registry.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The state one MCP session carries between tool calls, and the project path it resolves against.
// `tessera-mcp` is a stdio server started per client, so this is process-lifetime state and there
// is no cross-session sharing to synchronize: the serve loop is single-threaded and calls into it
// from one thread (src/harness/serve.cpp).
namespace tessera::mcp {

// Bounds the text one call may carry. The front end's own per-file limit is 1 MiB
// (docs/specs/jsx-subset-v1.md section 15); the adapter allows exactly that much so it never
// truncates a legal file, and rejects anything larger with a typed error instead of compiling a
// prefix (a prefix would report diagnostics for text the client did not send).
inline constexpr std::size_t kMaxSourceBytes = 1024 * 1024;

// Where the open project's root came from (docs/specs/designer-studio-v1.md section 8). The order
// of precedence is the order of this list: an explicit `open_project` beats the client's `roots`,
// which beat the server's working directory.
enum class RootSource {
    Argument,
    Roots,
    Cwd,
};

[[nodiscard]] std::string_view to_string(RootSource source) noexcept;

// The project a session has open. `directory` is the real host directory the file tools read; it is
// never put in a tool result (the server redacts absolute paths), which names only `folder` and the
// `source`.
struct OpenProject {
    std::string path; // as the client sent it, so it echoes back verbatim (empty for roots/cwd)
    std::string root; // normalized POSIX form used to resolve later relative paths
    std::filesystem::path directory;
    std::string folder; // the root's own folder name, for results
    RootSource source = RootSource::Argument;
};

class SessionState {
public:
    // Records `project` as open, or refuses with the reason. Refusing an unopenable path keeps
    // `open_project` from claiming success it cannot back: a later tool would resolve against a
    // directory that is not there.
    [[nodiscard]] std::optional<std::string> open_project(std::string path);

    // The project the file tools resolve against: the explicit or `roots` project when one is set,
    // else the server's working directory (resolved once and kept). Null only when even the working
    // directory cannot be read.
    [[nodiscard]] const OpenProject* resolved_project();

    // The project as it stands, without resolving the working directory. Null before any tool or
    // `roots` answer set one.
    [[nodiscard]] const std::optional<OpenProject>& project() const noexcept {
        return project_;
    }

    // `roots` support: the client declared the capability at `initialize`, and the roots it later
    // returned (`file://` URIs). The first usable root becomes the project unless the client
    // opened one explicitly; an empty or unusable list leaves the working-directory fallback.
    void set_client_roots(bool declared) noexcept {
        client_roots_ = declared;
    }
    [[nodiscard]] bool client_roots() const noexcept {
        return client_roots_;
    }
    void apply_roots(const std::vector<std::string>& uris);

    // The preview viewport `set_viewport` last set (`snapshot` renders at it). Defaults are the
    // preview's own, so a `snapshot` before any `set_viewport` matches the CLI's defaults.
    [[nodiscard]] ViewportState& viewport() noexcept {
        return viewport_;
    }
    [[nodiscard]] const ViewportState& viewport() const noexcept {
        return viewport_;
    }

private:
    std::optional<OpenProject> project_;
    ViewportState viewport_;
    bool client_roots_ = false;
};

// The host path a `file://` root URI names, or empty when it is not a usable local file URI
// (another scheme, a remote host, an undecodable escape).
[[nodiscard]] std::string path_from_file_uri(std::string_view uri);

// Normalizes `path` to the project-relative POSIX form the diagnostics document uses
// (docs/specs/designer-protocol-v1.md section 3.1): backslashes become slashes, `.` segments
// vanish, and leading slashes/`.` segments are dropped. Returns the empty string when the path
// escapes its root, which the caller reports rather than normalizing away.
[[nodiscard]] std::string normalize_project_path(std::string_view path);

} // namespace tessera::mcp
