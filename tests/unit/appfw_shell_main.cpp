// SPDX-License-Identifier: MIT
//
// Issue #450 (`shell` module) entry point. One executable per area, matching the repository's
// unit-test convention; the groups live in the sibling TUs:
//
//   * appfw_shell_path_tests.cpp       - the section 8 path rule (traversal, executable, size)
//   * appfw_shell_capability_tests.cpp - section 9.1 negotiation: Unsupported vs Denied, refresh
//   * appfw_shell_clipboard_tests.cpp  - section 2: round trip, bound, secure field, one at a time
//   * appfw_shell_dialog_tests.cpp     - section 5: open/save/folder, cancel, handles, path refusal
//   * appfw_shell_drop_tests.cpp       - section 3: payload kinds, budget, drop action as a value
//   * appfw_shell_task_tests.cpp       - the coroutine forms and their awaiter
//
// Every group is deterministic: `ManualClock`, `FakeShell` and a hand-drained `CompletionQueue`. No
// test in the suite opens a dialog, touches a real clipboard, reads a wall clock, sleeps, starts a
// thread or touches the file system.

#include "appfw_shell_test_support.hpp"

#include <iostream>

namespace shell_test {

int run_path_tests();
int run_capability_tests();
int run_clipboard_tests();
int run_dialog_tests();
int run_drop_tests();
int run_task_tests();

} // namespace shell_test

int main() {
    const int paths = shell_test::run_path_tests();
    const int capabilities = shell_test::run_capability_tests();
    const int clipboard = shell_test::run_clipboard_tests();
    const int dialogs = shell_test::run_dialog_tests();
    const int drops = shell_test::run_drop_tests();
    const int tasks = shell_test::run_task_tests();
    (void)paths;
    (void)capabilities;
    (void)clipboard;
    (void)dialogs;
    (void)drops;
    (void)tasks;

    if (shell_test::g_failures != 0) {
        std::cerr << "appfw_shell: " << shell_test::g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "appfw_shell: ok\n";
    return 0;
}
