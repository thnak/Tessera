// SPDX-License-Identifier: MIT

#include "tessera/appfw/shell_path.hpp"

#include <array>
#include <cstdint>

namespace tessera::appfw {
namespace {

// The refused extensions (section 8: "do not auto-open dropped executables"). Short and explicit on
// purpose: a deny-list of every executable form is impossible, so this is the set the module
// promises to refuse and a reviewer can read in one screen.
constexpr std::array<std::string_view, 14> kRefusedExtensions = {
    ".exe", ".com", ".bat",  ".cmd", ".msi", ".scr", ".pif",
    ".cpl", ".sh",  ".bash", ".zsh", ".ps1", ".vbs", ".js",
};

[[nodiscard]] char ascii_lower(char character) noexcept {
    if (character >= 'A' && character <= 'Z') {
        return static_cast<char>(character - 'A' + 'a');
    }
    return character;
}

[[nodiscard]] bool equals_ignore_case(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

// Both separators are treated as separators on both lanes: a Windows path in a fixture must be
// checked by the same rule on Linux, and a POSIX path on Windows.
[[nodiscard]] bool is_separator(char character) noexcept {
    return character == '/' || character == '\\';
}

} // namespace

bool shell_path_is_absolute(std::string_view path) noexcept {
    if (path.empty()) {
        return false;
    }
    if (is_separator(path.front())) {
        // A single leading separator is a POSIX root; a doubled one is a UNC share. Both absolute.
        return true;
    }
    // A drive letter: `C:` followed by a separator. `C:foo` is drive-relative on Windows and is
    // therefore *not* absolute, which is exactly the case that must not slip through.
    if (path.size() >= 3 && path[1] == ':') {
        const char drive = ascii_lower(path[0]);
        return drive >= 'a' && drive <= 'z' && is_separator(path[2]);
    }
    return false;
}

bool shell_path_has_traversal(std::string_view path) noexcept {
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = start;
        while (end < path.size() && !is_separator(path[end])) {
            ++end;
        }
        const std::string_view component = path.substr(start, end - start);
        if (component == "..") {
            return true;
        }
        if (end >= path.size()) {
            break;
        }
        start = end + 1;
    }
    return false;
}

bool shell_path_is_executable(std::string_view path) noexcept {
    // The extension is what follows the last separator's last dot. A dot in a directory name must
    // not be mistaken for one, so the search starts after the final separator.
    std::size_t name_start = 0;
    for (std::size_t index = 0; index < path.size(); ++index) {
        if (is_separator(path[index])) {
            name_start = index + 1;
        }
    }
    const std::string_view name = path.substr(name_start);
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return false;
    }
    const std::string_view extension = name.substr(dot);
    for (const std::string_view refused : kRefusedExtensions) {
        if (equals_ignore_case(extension, refused)) {
            return true;
        }
    }
    return false;
}

PathRejection shell_path_check(std::string_view path) noexcept {
    if (path.empty()) {
        return PathRejection::Empty;
    }
    if (path.size() > kMaxShellPathBytes) {
        return PathRejection::TooLong;
    }
    if (path.find('\0') != std::string_view::npos) {
        return PathRejection::ContainsNul;
    }
    if (shell_path_has_traversal(path)) {
        return PathRejection::Traversal;
    }
    if (!shell_path_is_absolute(path)) {
        return PathRejection::Relative;
    }
    if (shell_path_is_executable(path)) {
        return PathRejection::Executable;
    }
    return PathRejection::None;
}

std::size_t shell_refused_extension_count() noexcept {
    return kRefusedExtensions.size();
}

std::string_view shell_refused_extension(std::size_t index) noexcept {
    return index < kRefusedExtensions.size() ? kRefusedExtensions[index] : std::string_view{};
}

} // namespace tessera::appfw
