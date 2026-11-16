// SPDX-License-Identifier: MIT

#pragma once

#include "preview_tools.hpp"
#include "tessera/hal/fold_posture.hpp"
#include "tool_registry.hpp"

#include <cstddef>
#include <optional>
#include <string>

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

// The project a session has open. Today only the path and the validation that it names a directory
// exist: the compile-only tools take their text in the request, so nothing here reads a file yet.
// That is deliberate, not an omission -- a tool that read the project would have to be a file
// reader with its own security surface (designer-studio-v1.md section 10), which is the D-S7 slice.
struct OpenProject {
    std::string path; // as the client sent it, so it echoes back verbatim
    std::string root; // normalized POSIX form used to resolve later relative paths
};

class SessionState {
public:
    // Records `project` as open, or refuses with the reason. Refusing an unopenable path keeps
    // `open_project` from claiming success it cannot back: a later tool would resolve against a
    // directory that is not there.
    [[nodiscard]] std::optional<std::string> open_project(std::string path);

    [[nodiscard]] const std::optional<OpenProject>& project() const noexcept {
        return project_;
    }

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
};

// Normalizes `path` to the project-relative POSIX form the diagnostics document uses
// (docs/specs/designer-protocol-v1.md section 3.1): backslashes become slashes, `.` segments
// vanish, and leading slashes/`.` segments are dropped. Returns the empty string when the path
// escapes its root, which the caller reports rather than normalizing away.
[[nodiscard]] std::string normalize_project_path(std::string_view path);

} // namespace tessera::mcp
