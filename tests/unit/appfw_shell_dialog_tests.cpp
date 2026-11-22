// SPDX-License-Identifier: MIT
//
// Dialogs and capability handles (docs/specs/platform-shell-v1.md sections 5, 8 and 9.1; issue
// #450): open/save/folder, cancel as a value, the path rule applied to what the platform returned,
// and the handle token that makes a `FileHandle` unforgeable.

#include "appfw_shell_test_support.hpp"

namespace shell_test {
namespace {

void test_open_returns_handles() {
    Harness harness;
    harness.fake.push_dialog_paths({valid_path("a.txt"), valid_path("b.md")});

    std::vector<FileHandle> handles;
    bool done = false;
    auto submitted = harness.service.submit_open_dialog(
        open_spec(), [&](std::expected<std::vector<FileHandle>, ShellError> result) {
            done = true;
            expect(result.has_value(), "the open dialog succeeded");
            if (result.has_value()) {
                handles = std::move(*result);
            }
        });
    expect(submitted.has_value(), "the open dialog was accepted");
    expect(!done, "the dialog completion does not run inline");
    pump(harness);
    expect(done, "the dialog completed after a pump");
    expect(handles.size() == 2, "both selected files came back");
    for (const FileHandle& handle : handles) {
        expect(handle.valid(), "every handle is valid");
        expect(harness.service.validate(handle), "every handle validates against its service");
    }
    expect(harness.service.live_handles() == 2, "both handles are live");
}

void test_cancel_is_a_value() {
    Harness harness;
    harness.fake.push_dialog_cancel();
    bool done = false;
    (void)harness.service.submit_open_dialog(
        open_spec(), [&](std::expected<std::vector<FileHandle>, ShellError> result) {
            done = true;
            // Section 5: cancellation is first-class, so an empty result is a success.
            expect(result.has_value(), "a cancelled dialog is not an error");
            if (result.has_value()) {
                expect(result->empty(), "a cancelled dialog returns no handles");
            }
        });
    pump(harness);
    expect(done, "the cancelled dialog completed");
    expect(harness.service.diagnostics().dialogs_cancelled == 1, "the cancel is counted");
}

void test_save_and_folder() {
    Harness harness;
    harness.fake.push_dialog_paths({valid_path("saved.txt")});
    SaveDialogSpec save;
    save.title = "Save";
    save.suggested_name = "saved.txt";
    std::vector<FileHandle> saved;
    (void)harness.service.submit_save_dialog(
        save, [&](std::expected<std::vector<FileHandle>, ShellError> result) {
            expect(result.has_value(), "the save dialog succeeded");
            if (result.has_value()) {
                saved = std::move(*result);
            }
        });
    pump(harness);
    expect(saved.size() == 1, "the save dialog returned its path");

    harness.fake.push_dialog_paths({"/tmp/tessera-shell"});
    FolderDialogSpec folder;
    folder.title = "Pick";
    std::vector<FileHandle> picked;
    (void)harness.service.submit_folder_dialog(
        folder, [&](std::expected<std::vector<FileHandle>, ShellError> result) {
            expect(result.has_value(), "the folder dialog succeeded");
            if (result.has_value()) {
                picked = std::move(*result);
            }
        });
    pump(harness);
    expect(picked.size() == 1, "the folder dialog returned its path");

    // The back end saw the right dialog kind for each, which is what proves the dispatch is not
    // collapsing all three onto one path.
    const auto& received = harness.fake.received();
    expect(received.size() == 2, "the back end saw both dialogs");
    expect(received[0].dialog_kind == DialogKind::Save,
           "the save dialog reached the back end as Save");
    expect(received[1].dialog_kind == DialogKind::Folder,
           "the folder dialog reached the back end as Folder");
}

void test_a_bad_path_from_the_platform_is_refused() {
    Harness harness;
    // A back end that returns a traversal path is caught by the module, which is the whole reason
    // validation is not the back end's job.
    harness.fake.push_dialog_paths({"/tmp/../etc/passwd"});
    bool done = false;
    (void)harness.service.submit_open_dialog(
        open_spec(), [&](std::expected<std::vector<FileHandle>, ShellError> result) {
            done = true;
            expect(!result.has_value(), "a traversal path is refused");
            if (!result.has_value()) {
                expect(result.error().kind == ShellErrorKind::InvalidPath,
                       "the refusal is InvalidPath");
            }
        });
    pump(harness);
    expect(done, "the refusal was delivered");
    expect(harness.service.live_handles() == 0, "no handle was issued for a refused path");
    expect(harness.service.diagnostics().refused_invalid_path == 1, "the refusal is counted");
}

void test_an_executable_from_the_platform_is_refused() {
    Harness harness;
    harness.fake.push_dialog_paths({valid_path("setup.exe")});
    bool done = false;
    (void)harness.service.submit_open_dialog(
        open_spec(), [&](std::expected<std::vector<FileHandle>, ShellError> result) {
            done = true;
            expect(!result.has_value() && result.error().kind == ShellErrorKind::InvalidPath,
                   "an executable is refused");
        });
    pump(harness);
    expect(done, "the refusal was delivered");
}

void test_one_refused_path_fails_the_whole_result() {
    Harness harness;
    // Three files, the middle one refused. A caller that got two of three would have no way to know
    // which one was dropped, so the whole result fails.
    harness.fake.push_dialog_paths(
        {valid_path("a.txt"), "/tmp/../etc/passwd", valid_path("c.txt")});
    bool done = false;
    (void)harness.service.submit_open_dialog(
        open_spec(), [&](std::expected<std::vector<FileHandle>, ShellError> result) {
            done = true;
            expect(!result.has_value(), "one refused path fails the result");
        });
    pump(harness);
    expect(done, "the refusal was delivered");
    expect(harness.service.live_handles() == 0, "no partial handles were issued");
}

void test_handle_tokens() {
    Harness harness;
    harness.fake.push_dialog_paths({valid_path("a.txt")});
    std::vector<FileHandle> handles;
    (void)harness.service.submit_open_dialog(
        open_spec(), [&](std::expected<std::vector<FileHandle>, ShellError> result) {
            if (result.has_value()) {
                handles = std::move(*result);
            }
        });
    pump(harness);
    expect(handles.size() == 1, "one handle came back");
    if (handles.empty()) {
        return;
    }
    const FileHandle handle = handles.front();

    // A forged token is not valid.
    FileHandle forged;
    forged.path = handle.path;
    forged.token = handle.token + 1000;
    expect(!harness.service.validate(forged), "a forged token does not validate");

    // A real token with a different path is not valid either: the pair is what is checked.
    FileHandle swapped;
    swapped.path = valid_path("other.txt");
    swapped.token = handle.token;
    expect(!harness.service.validate(swapped), "a token with a different path does not validate");

    // An invalid handle is not valid.
    expect(!harness.service.validate(FileHandle{}), "an empty handle does not validate");

    // Revoking makes the real handle stop validating.
    expect(harness.service.revoke(handle), "the handle can be revoked");
    expect(!harness.service.validate(handle), "a revoked handle does not validate");
    expect(!harness.service.revoke(handle), "a second revoke is a no-op");
    expect(harness.service.live_handles() == 0, "the revoked handle is gone");
}

void test_a_handle_from_another_service_does_not_validate() {
    Harness first;
    first.fake.push_dialog_paths({valid_path("a.txt")});
    std::vector<FileHandle> handles;
    (void)first.service.submit_open_dialog(
        open_spec(), [&](std::expected<std::vector<FileHandle>, ShellError> result) {
            if (result.has_value()) {
                handles = std::move(*result);
            }
        });
    pump(first);
    expect(handles.size() == 1, "the first service issued a handle");

    // A second service starts its token counter at the same value, so the token alone must not be
    // enough: the pair is checked against *this* service's table.
    Harness second;
    if (!handles.empty()) {
        expect(!second.service.validate(handles.front()),
               "a handle from another service does not validate");
    }
}

void test_cancel_dialog() {
    Harness harness;
    harness.fake.push_dialog_paths({valid_path("a.txt")}, 100);
    auto submitted = harness.service.submit_open_dialog(open_spec(), {});
    expect(submitted.has_value(), "the dialog is accepted");
    expect(harness.service.cancel_dialog(), "the in-flight dialog can be cancelled");
    expect(!harness.service.cancel_dialog(), "a second cancel is a no-op");
    advance(harness, 0.2);
    expect(harness.fake.cancelled_operations() == 1, "the back end observed the cancellation");
}

} // namespace

int run_dialog_tests() {
    test_open_returns_handles();
    test_cancel_is_a_value();
    test_save_and_folder();
    test_a_bad_path_from_the_platform_is_refused();
    test_an_executable_from_the_platform_is_refused();
    test_one_refused_path_fails_the_whole_result();
    test_handle_tokens();
    test_a_handle_from_another_service_does_not_validate();
    test_cancel_dialog();
    return g_failures;
}

} // namespace shell_test
