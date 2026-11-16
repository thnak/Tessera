// SPDX-License-Identifier: MIT
//
// AF7 (issue #179): the atomic-replace sequence, and the mid-write failure it exists to survive.
//
// Every test here is an experiment with one variable: the step of the sequence that fails. The fake
// file system makes that reproducible, and the assertions are about what a *reader* would see - the
// target's old bytes or its new bytes, never a mixture and never nothing.

#include "appfw_storage_test_support.hpp"

#include <string>
#include <vector>

namespace storage_test {
namespace {

using tessera::appfw::atomic_replace;
using tessera::appfw::AtomicWriteTrace;
using tessera::appfw::is_valid_entry_name;
using tessera::appfw::temp_path_for;

// The happy path, and the order that makes it atomic: write the temp, flush it, then - only then -
// replace. The assertion is on the full call vector, so a reordering cannot pass silently.
void the_sequence_writes_temp_flushes_then_replaces() {
    FakeFileSystem files;
    files.files["/store/doc"] = "old";

    AtomicWriteTrace trace;
    const auto result =
        atomic_replace(files, "/store/doc", "new", Durability::BestEffort, {}, 1, &trace);

    expect(result.has_value(), "atomic replace succeeds");
    expect(files.content_of("/store/doc") == "new", "target holds the new content");
    expect(trace.temp_written && trace.temp_flushed && trace.replaced,
           "trace records write-temp, flush-temp, replace");
    expect(!trace.directory_flushed, "BestEffort does not flush the directory");

    const std::vector<std::string> steps = files.steps();
    const std::vector<std::string> expected{"create-directory", "write", "flush", "replace"};
    expect(steps == expected, "step order is create-directory, write, flush, replace");
    // The temp file is in the target's own directory: the rename is a same-device rename.
    expect(files.temp_file_count() == 0, "no temp file survives a successful write");
    expect(temp_path_for("/store/doc", 1).rfind("/store/", 0) == 0,
           "temp path is in the target's directory");
    expect(!files.has_file(temp_path_for("/store/doc", 1)),
           "the temp path was consumed by the rename");
}

// The requirement: a failure in the middle of the write leaves the *old* content intact. Each step
// is a separate case so the test names which failure it survived.
void a_failure_at_each_step_leaves_the_old_content() {
    struct Case {
        FailStep step;
        const char* name;
    };
    const Case cases[] = {
        {FailStep::CreateDirectory, "create-directory"},
        {FailStep::Write, "write-temp"},
        {FailStep::Flush, "flush-temp"},
        {FailStep::Replace, "replace"},
    };

    for (const Case& test_case : cases) {
        FakeFileSystem files;
        files.files["/store/doc"] = "old";
        files.fail = test_case.step;

        const auto result =
            atomic_replace(files, "/store/doc", "brand new content", Durability::BestEffort, {}, 7);

        expect(!result.has_value(), test_case.name);
        expect(files.content_of("/store/doc") == "old", "old content intact after failure");
        expect(files.temp_file_count() == 0, "no temp file left behind after failure");
    }
}

// The failure the task names explicitly: the write itself fails after putting bytes on disk. The
// existing target must be untouched and the partial temp file must be removed.
void a_torn_temp_write_never_reaches_the_target() {
    FakeFileSystem files;
    files.files["/store/doc"] = "the original";
    files.fail = FailStep::Write;
    files.leave_partial_on_write_failure = true;

    const auto result =
        atomic_replace(files, "/store/doc", "the replacement", Durability::BestEffort, {}, 3);

    expect(!result.has_value(), "torn temp write reports failure");
    expect(result.error().code == StorageErrorCode::Io, "reported as an io failure");
    expect(result.error().step == "write-temp", "the failing step is named");
    expect(files.content_of("/store/doc") == "the original",
           "target unchanged by a torn temp write");
    expect(files.temp_file_count() == 0, "the partial temp file was removed");
}

// A target that did not exist is still absent after a failed write: "no partial file" covers the
// create case, not only the replace case.
void a_failed_first_write_leaves_no_target() {
    FakeFileSystem files;
    files.fail = FailStep::Replace;

    const auto result =
        atomic_replace(files, "/store/fresh", "content", Durability::BestEffort, {}, 2);

    expect(!result.has_value(), "first write fails at replace");
    expect(!files.has_file("/store/fresh"), "no target file was created");
    expect(files.temp_file_count() == 0, "no temp file left behind");
}

// `Durable` adds exactly one step - the directory flush - and it happens after the replace.
void durable_adds_the_directory_flush_after_the_replace() {
    FakeFileSystem files;
    files.files["/store/doc"] = "old";

    AtomicWriteTrace trace;
    const auto result =
        atomic_replace(files, "/store/doc", "new", Durability::Durable, {}, 1, &trace);

    expect(result.has_value(), "durable replace succeeds");
    expect(trace.directory_flushed, "trace records the directory flush");
    const std::vector<std::string> steps = files.steps();
    const std::vector<std::string> expected{"create-directory", "write", "flush", "replace",
                                            "flush-directory"};
    expect(steps == expected, "durable order ends with flush-directory");
}

// A directory-flush failure is reported even though the replace already happened: the caller asked
// for durability and did not get it.
void a_directory_flush_failure_is_reported() {
    FakeFileSystem files;
    files.files["/store/doc"] = "old";
    files.fail = FailStep::FlushDirectory;

    const auto result = atomic_replace(files, "/store/doc", "new", Durability::Durable, {}, 1);

    expect(!result.has_value(), "durable write reports the flush failure");
    expect(result.error().step == "flush-dir", "the failing step is the directory flush");
    expect(files.content_of("/store/doc") == "new",
           "the new content is visible: the replace already happened");
}

// Two writes to the same target use different temp names, so a slow writer cannot clobber a faster
// one's temp file before its rename.
void generations_give_distinct_temp_paths() {
    const std::string first = temp_path_for("/store/doc", 1);
    const std::string second = temp_path_for("/store/doc", 2);
    expect(first != second, "each generation has its own temp path");
    expect(first.rfind("/store/doc.", 0) == 0, "the temp path is named after the target");
    expect(first.size() > 4 && first.substr(first.size() - 4) == ".tmp",
           "the temp path ends in .tmp");
}

void entry_names_are_sandboxed() {
    expect(is_valid_entry_name("settings"), "a plain name is valid");
    expect(is_valid_entry_name("user.settings.v2"), "dots inside a name are valid");
    expect(!is_valid_entry_name(""), "an empty name is invalid");
    expect(!is_valid_entry_name("."), "a single dot is invalid");
    expect(!is_valid_entry_name(".."), "a double dot is invalid");
    expect(!is_valid_entry_name("../escape"), "a traversal prefix is invalid");
    expect(!is_valid_entry_name("a/../../b"), "an embedded traversal is invalid");
    expect(!is_valid_entry_name("a/b"), "a forward slash is invalid");
    expect(!is_valid_entry_name("a\\b"), "a backslash is invalid");
    expect(!is_valid_entry_name("C:evil"), "a drive prefix is invalid");
    expect(!is_valid_entry_name("/absolute"), "an absolute name is invalid");

    // Identity, not just traversal (security review finding S5). A name a filesystem silently
    // normalises can address a *different* entry, turning a write into a silent overwrite of
    // unrelated data -- the one outcome a store must never produce.
    expect(!is_valid_entry_name("trail."), "a trailing dot is invalid: NTFS strips it");
    expect(!is_valid_entry_name("trail "), "a trailing space is invalid: Win32 strips it");
    expect(!is_valid_entry_name("nul"), "a device name is invalid: the write goes nowhere");
    expect(!is_valid_entry_name("NUL"), "device names are matched case-insensitively");
    expect(!is_valid_entry_name("con.txt"), "a device name with an extension is still the device");
    expect(!is_valid_entry_name("com1"), "COM1 is a device");
    expect(!is_valid_entry_name("lpt9.log"), "LPT9 with an extension is still the device");
    expect(!is_valid_entry_name(std::string("a\x01b")), "a control character is invalid");

    // And these must keep working: the fix must not over-reach.
    expect(is_valid_entry_name("trail"), "the name without the dot is still valid");
    expect(is_valid_entry_name("console"), "a device-like word is not a device");
    expect(!is_valid_entry_name("nul.txt.bak"),
           "only the stem before the first dot matters, so this is still the device");
    expect(is_valid_entry_name("nullish"), "a longer word starting with a device name is fine");
    expect(is_valid_entry_name("nav.state"), "a normal dotted name is valid");
    expect(is_valid_entry_name("Settings"), "an upper-case name is still valid (case policy is the "
                                            "spec's call, not a silent security change)");
}

} // namespace

int run_atomic_tests() {
    the_sequence_writes_temp_flushes_then_replaces();
    a_failure_at_each_step_leaves_the_old_content();
    a_torn_temp_write_never_reaches_the_target();
    a_failed_first_write_leaves_no_target();
    durable_adds_the_directory_flush_after_the_replace();
    a_directory_flush_failure_is_reported();
    generations_give_distinct_temp_paths();
    entry_names_are_sandboxed();
    return g_failures;
}

} // namespace storage_test
