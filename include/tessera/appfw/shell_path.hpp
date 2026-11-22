// SPDX-License-Identifier: MIT
//
// Path validation for the shell module (docs/specs/platform-shell-v1.md section 8; issue #450).
//
// Section 8 is the security boundary of this module, and it is the one rule that must not be
// re-implemented per back end: "Validate file paths, URIs, sizes, MIME types; reject
// symlink/traversal attacks" and "Do not auto-open dropped executables". A back end that validated
// paths itself would be a second, divergent copy of this rule, so validation lives here and the
// back end never sees an unvalidated path.
//
// **What "valid" means.** A path is accepted when all of these hold:
//
//   1. it is non-empty and within `kMaxShellPathBytes`;
//   2. it contains no NUL byte (a truncated C string is a classic confusion);
//   3. it has no `..` component, so a drop cannot walk out of the directory it names;
//   4. it is absolute, because a relative path's meaning depends on a working directory the app
//   does
//      not own;
//   5. it does not name an executable or a script by extension, which is section 8's "do not
//      auto-open dropped executables".
//
// **What it deliberately does not do.** It does not touch the file system: no `stat`, no
// `realpath`, no symlink resolution. That is a decision, not an omission. A symlink check needs a
// syscall, a syscall is a block, and this function is called on the UI thread while a drop is being
// resolved. The back end - which already runs on an `IoTaskExecutor` worker - is where a resolved
// path is opened, and it is the platform's own open that refuses a symlink escape. What this
// function guarantees is that the *string* the app is handed cannot be a traversal or an
// executable, which is the part that can be decided without I/O.
//
// The extension list is deliberately short and explicit rather than a "known bad" list: a deny-list
// of executable extensions is always incomplete, so the rule is "these are the ones we refuse" and
// the list is a constant a reviewer can read.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tessera::appfw {

// The longest path the module accepts. Windows' own `MAX_PATH` is 260 for the legacy API and 32767
// for the `\\?\` form; 4096 is the POSIX `PATH_MAX` and comfortably covers both, so one bound
// serves every lane.
inline constexpr std::size_t kMaxShellPathBytes = 4096;

// Why a path was refused. Stable strings, so a diagnostic and a test can assert on them.
enum class PathRejection : std::uint8_t {
    None = 0,
    Empty,
    TooLong,
    ContainsNul,
    Traversal,  // a `..` component
    Relative,   // not absolute
    Executable, // a refused extension
};

[[nodiscard]] constexpr std::string_view path_rejection_name(PathRejection rejection) noexcept {
    switch (rejection) {
    case PathRejection::None:
        return "none";
    case PathRejection::Empty:
        return "empty";
    case PathRejection::TooLong:
        return "too-long";
    case PathRejection::ContainsNul:
        return "contains-nul";
    case PathRejection::Traversal:
        return "traversal";
    case PathRejection::Relative:
        return "relative";
    case PathRejection::Executable:
        return "executable";
    }
    return "none";
}

// True when `path` is absolute on either lane: a POSIX leading `/`, a Windows drive letter
// (`C:\` or `C:/`), or a UNC prefix (`\\server\share`). Both forms are accepted on both lanes
// because a test asserts the rule once and a Windows path in a fixture must not fail on Linux.
[[nodiscard]] bool shell_path_is_absolute(std::string_view path) noexcept;

// True when `path` has a `..` component. A `..` inside a file name (`a..b.txt`) is not a component
// and is accepted.
[[nodiscard]] bool shell_path_has_traversal(std::string_view path) noexcept;

// True when `path`'s extension is one this module refuses to hand to a caller. Case-insensitive.
[[nodiscard]] bool shell_path_is_executable(std::string_view path) noexcept;

// The whole rule, in the order the checks run. `None` means accepted.
[[nodiscard]] PathRejection shell_path_check(std::string_view path) noexcept;

// The number of extensions `shell_path_is_executable` refuses, and the list itself, so a test can
// assert the list rather than a sample of it.
[[nodiscard]] std::size_t shell_refused_extension_count() noexcept;
[[nodiscard]] std::string_view shell_refused_extension(std::size_t index) noexcept;

} // namespace tessera::appfw
