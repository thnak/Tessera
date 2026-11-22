// SPDX-License-Identifier: MIT
//
// The coroutine forms (docs/specs/platform-shell-v1.md sections 2 and 5; issue #450). Section 7
// says every operation is an async task whose result lands on the UI thread, so the
// `Task`-returning `read_clipboard`/`open_file` forms are the spec's own shape and are tested
// through the real awaiter rather than only through the callback form.

#include "appfw_shell_test_support.hpp"

namespace shell_test {
namespace {

void test_read_clipboard_task() {
    Harness harness;
    harness.fake.push_clipboard_read(ClipboardContent{"from a task"});
    std::expected<std::optional<ClipboardContent>, ShellError> result{
        std::unexpected(ShellError{ShellErrorKind::Failed, "never ran"})};
    const bool finished = run_task(harness, harness.service.read_clipboard(false), result);
    expect(finished, "the read task finished without an exception");
    expect(result.has_value(), "the read task succeeded");
    if (result.has_value()) {
        expect(result->has_value() && (*result)->text == "from a task",
               "the read task carried the clipboard text");
    }
}

void test_read_clipboard_task_refusal() {
    Harness harness({}, ShellCapabilities{});
    std::expected<std::optional<ClipboardContent>, ShellError> result;
    const bool finished = run_task(harness, harness.service.read_clipboard(false), result);
    expect(finished, "a refused read task still finishes");
    expect(!result.has_value() && result.error().kind == ShellErrorKind::Unsupported,
           "a refused read task carries the error as a value");
}

void test_write_clipboard_task() {
    Harness harness;
    harness.fake.push_clipboard_write(true);
    std::expected<void, ShellError> result{
        std::unexpected(ShellError{ShellErrorKind::Failed, "never ran"})};
    const bool finished =
        run_task(harness, harness.service.write_clipboard(ClipboardContent{"task write"}), result);
    expect(finished, "the write task finished");
    expect(result.has_value(), "the write task succeeded");
    expect(harness.fake.received().size() == 1, "the back end saw the write");
}

void test_open_file_task() {
    Harness harness;
    harness.fake.push_dialog_paths({valid_path("a.txt"), valid_path("b.txt")});
    std::expected<std::vector<FileHandle>, ShellError> result;
    const bool finished = run_task(harness, harness.service.open_file(open_spec()), result);
    expect(finished, "the open task finished");
    expect(result.has_value(), "the open task succeeded");
    if (result.has_value()) {
        expect(result->size() == 2, "the open task carried both handles");
        for (const FileHandle& handle : *result) {
            expect(harness.service.validate(handle), "a task-issued handle validates");
        }
    }
}

void test_open_file_task_cancel() {
    Harness harness;
    harness.fake.push_dialog_cancel();
    std::expected<std::vector<FileHandle>, ShellError> result;
    const bool finished = run_task(harness, harness.service.open_file(open_spec()), result);
    expect(finished, "a cancelled open task finishes");
    // Cancellation is a value, not a throw: the task's own token is empty on purpose.
    expect(result.has_value() && result->empty(), "a cancelled open task returns an empty result");
}

void test_a_task_does_not_run_until_started() {
    Harness harness;
    harness.fake.push_clipboard_read(ClipboardContent{"lazy"});
    auto task = harness.service.read_clipboard(false);
    expect(!task.started(), "a task is lazy");
    expect(harness.fake.received().empty(), "a lazy task has not reached the back end");
    // Starting it is what submits the operation.
    expect(task.start(), "the task can be started");
    pump(harness);
    expect(task.done(), "the started task completed");
}

} // namespace

int run_task_tests() {
    test_read_clipboard_task();
    test_read_clipboard_task_refusal();
    test_write_clipboard_task();
    test_open_file_task();
    test_open_file_task_cancel();
    test_a_task_does_not_run_until_started();
    return g_failures;
}

} // namespace shell_test
