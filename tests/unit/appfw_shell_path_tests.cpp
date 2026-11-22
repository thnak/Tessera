// SPDX-License-Identifier: MIT
//
// The path rule (docs/specs/platform-shell-v1.md section 8; issue #450). Section 8 is the security
// boundary of this module, so it gets its own TU: a failure here names the rule, not a dialog.
//
// The cases are the ones section 8 names in prose - traversal, symlink/executable, size - plus the
// two that a naive implementation gets wrong: a `..` inside a file name (`a..b.txt`) is not a
// component, and a drive-relative Windows path (`C:foo`) is not absolute.

#include "appfw_shell_test_support.hpp"

namespace shell_test {
namespace {

void test_absolute_forms() {
    expect(appfw::shell_path_is_absolute("/tmp/file.txt"), "a POSIX root is absolute");
    expect(appfw::shell_path_is_absolute("C:\\Users\\file.txt"), "a drive path is absolute");
    expect(appfw::shell_path_is_absolute("C:/Users/file.txt"), "a drive path with / is absolute");
    expect(appfw::shell_path_is_absolute("\\\\server\\share\\file.txt"), "a UNC path is absolute");
    expect(!appfw::shell_path_is_absolute("file.txt"), "a bare name is not absolute");
    expect(!appfw::shell_path_is_absolute("./file.txt"), "a dot-relative path is not absolute");
    // The case that must not slip through: `C:foo` is drive-relative on Windows.
    expect(!appfw::shell_path_is_absolute("C:foo"), "a drive-relative path is not absolute");
    expect(!appfw::shell_path_is_absolute(""), "an empty path is not absolute");
}

void test_traversal() {
    expect(appfw::shell_path_has_traversal("/tmp/../etc/passwd"), "a .. component is traversal");
    expect(appfw::shell_path_has_traversal("..\\windows\\system32"), "a .. component is traversal");
    expect(appfw::shell_path_has_traversal("/tmp/a/../../b"), "a nested .. is traversal");
    // Not a component: the dots are part of the name.
    expect(!appfw::shell_path_has_traversal("/tmp/a..b.txt"),
           "a .. inside a name is not traversal");
    expect(!appfw::shell_path_has_traversal("/tmp/..."), "three dots is not a .. component");
    expect(!appfw::shell_path_has_traversal("/tmp/file.txt"), "a plain path has no traversal");
}

void test_executable_extensions() {
    expect(appfw::shell_path_is_executable("/tmp/setup.exe"), ".exe is refused");
    expect(appfw::shell_path_is_executable("/tmp/SETUP.EXE"),
           "the extension check is case-insensitive");
    expect(appfw::shell_path_is_executable("/tmp/run.sh"), ".sh is refused");
    expect(appfw::shell_path_is_executable("/tmp/script.ps1"), ".ps1 is refused");
    expect(!appfw::shell_path_is_executable("/tmp/notes.txt"), ".txt is accepted");
    expect(!appfw::shell_path_is_executable("/tmp/archive.tar.gz"), ".gz is accepted");
    // A dot in a directory name must not be read as the file's extension.
    expect(!appfw::shell_path_is_executable("/tmp/a.b/notes"),
           "a dotted directory is not an extension");
    expect(!appfw::shell_path_is_executable("/tmp/noext"), "a name with no dot is accepted");
    // The list is asserted, not sampled: a test that only checked .exe would pass a list that lost
    // every other entry.
    expect(appfw::shell_refused_extension_count() >= 10, "the refused list is not empty");
    for (std::size_t index = 0; index < appfw::shell_refused_extension_count(); ++index) {
        const std::string_view extension = appfw::shell_refused_extension(index);
        expect(!extension.empty() && extension.front() == '.',
               "every refused extension starts with a dot");
        expect(appfw::shell_path_is_executable(std::string("/tmp/file") + std::string(extension)),
               "every listed extension is refused");
    }
}

void test_check_order() {
    expect(appfw::shell_path_check("") == PathRejection::Empty, "empty is reported first");
    expect(appfw::shell_path_check(std::string(appfw::kMaxShellPathBytes + 1, 'a')) ==
               PathRejection::TooLong,
           "an over-long path is TooLong");
    expect(appfw::shell_path_check(std::string("/tmp/a\0b", 8)) == PathRejection::ContainsNul,
           "an embedded NUL is refused");
    expect(appfw::shell_path_check("/tmp/../etc/passwd") == PathRejection::Traversal,
           "traversal is reported");
    expect(appfw::shell_path_check("relative/file.txt") == PathRejection::Relative,
           "a relative path is reported");
    expect(appfw::shell_path_check("/tmp/setup.exe") == PathRejection::Executable,
           "an executable is reported");
    expect(appfw::shell_path_check("/tmp/notes.txt") == PathRejection::None,
           "a good path is accepted");
    expect(appfw::shell_path_check("C:\\Users\\notes.txt") == PathRejection::None,
           "a Windows path is accepted");
}

} // namespace

int run_path_tests() {
    test_absolute_forms();
    test_traversal();
    test_executable_extensions();
    test_check_order();
    return g_failures;
}

} // namespace shell_test
