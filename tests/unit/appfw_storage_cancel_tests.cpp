// SPDX-License-Identifier: MIT
//
// AF7 (issue #179): cancellation. A write cancelled mid-flight must not leave a temp file behind
// and must not corrupt the existing target.
//
// "Mid-flight" has two distinct meanings here, and both are covered:
//
//   * cancelled *before* the work body starts (the executor drops it): the file system is never
//     touched at all;
//   * cancelled *while* the body is between steps (the token fires from inside the sequence): the
//     temp file is removed and the target keeps its old bytes.
//
// The second case is the interesting one, and it is induced deterministically: the fake's `flush`
// cancels the token, which is exactly the moment the sequence checks it - after the temp file
// exists and before the rename.

#include "appfw_storage_test_support.hpp"

#include <string>

namespace storage_test {
namespace {

using tessera::appfw::atomic_replace;
using tessera::appfw::AtomicWriteTrace;

// Cancelled before the body runs: nothing happens at all, not even a temp write.
void a_write_cancelled_before_it_starts_touches_nothing() {
    Harness harness;
    harness.files.files["/store/doc"] = "old";
    harness.files.calls.clear();

    CancellationSource source;
    source.cancel();

    bool invoked = false;
    const bool posted =
        harness.store.save("doc", "new", source.token(), [&](auto) { invoked = true; });
    expect(!posted, "a cancelled submit is not posted");
    expect(!invoked, "the completion is not invoked for an unposted operation");
    harness.run_all();
    expect(harness.files.calls.empty(), "the file system was never touched");
    expect(harness.files.content_of("/store/doc") == "old",
           "the target still holds the old content");
}

// Cancelled from inside the sequence, after the temp file is written and flushed: the temp file is
// removed and the target keeps its old bytes. This is the "cancelled mid-flight" acceptance case.
void a_write_cancelled_mid_sequence_removes_its_temp_file() {
    FakeFileSystem files;
    files.files["/store/doc"] = "old";

    CancellationSource source;
    // The flush step is where the sequence checks the token, so that is where the cancel is
    // injected: after the temp write, before the replace.
    files.on_flush = [&source] { source.cancel(); };

    AtomicWriteTrace trace;
    const auto result = atomic_replace(files, "/store/doc", "new", Durability::BestEffort,
                                       source.token(), 1, &trace);

    expect(!result.has_value(), "a cancelled mid-sequence write reports failure");
    expect(result.error().code == StorageErrorCode::Cancelled, "reported as Cancelled");
    expect(result.error().step == "cancelled", "the step is the cancellation point");
    expect(trace.temp_written, "the temp file had been written before the cancel");
    expect(trace.temp_removed, "the temp file was removed");
    expect(!trace.replaced, "the replace never happened");
    expect(files.content_of("/store/doc") == "old", "the target keeps its old content");
    expect(files.temp_file_count() == 0, "no temp file is left behind");
}

// The same cancellation through the store's public API: the completion receives `Cancelled`, the
// store counts the aborted write exactly once, and the target is unchanged.
void a_cancelled_store_save_reports_cancelled_and_cleans_up() {
    Harness harness;
    harness.files.files["/store/notes"] = "previous";

    CancellationSource source;
    harness.files.on_flush = [&source] { source.cancel(); };

    std::expected<void, StorageError> delivered;
    bool invoked = false;
    const bool posted = harness.store.save("notes", "replacement", source.token(),
                                           [&](std::expected<void, StorageError> status) {
                                               invoked = true;
                                               delivered = status;
                                           });
    expect(posted, "the save is posted");
    harness.run_one();

    expect(invoked, "the completion ran");
    expect(!delivered.has_value(), "the completion reports failure");
    expect(delivered.error().code == StorageErrorCode::Cancelled, "the failure is Cancelled");
    expect(harness.files.content_of("/store/notes") == "previous",
           "the stored content is unchanged");
    expect(harness.files.temp_file_count() == 0, "no temp file remains");

    const auto diagnostics = harness.store.diagnostics();
    expect(diagnostics.aborted_writes == 1, "one aborted write recorded");
    expect(diagnostics.writes == 0, "no successful write recorded");
}

// A cancelled read never returns stale bytes as if they were the value.
void a_cancelled_read_reports_cancelled() {
    Harness harness;
    harness.files.files["/store/doc"] = "payload";
    harness.files.calls.clear();

    CancellationSource source;
    source.cancel();

    bool invoked = false;
    const bool posted = harness.store.load("doc", source.token(), [&](auto) { invoked = true; });
    expect(!posted, "a cancelled load is not posted");
    expect(!invoked, "no completion for an operation that never started");
    expect(harness.files.calls.empty(), "the read never reached the file system");
}

// A token that is cancelled between the temp write and the replace on the *first* write to a name
// leaves no target at all, rather than an empty or partial one.
void a_cancelled_first_write_leaves_no_target() {
    FakeFileSystem files;
    CancellationSource source;
    files.on_flush = [&source] { source.cancel(); };

    const auto result =
        atomic_replace(files, "/store/fresh", "content", Durability::BestEffort, source.token(), 4);

    expect(!result.has_value(), "the cancelled first write fails");
    expect(!files.has_file("/store/fresh"), "no target file exists");
    expect(files.temp_file_count() == 0, "no temp file remains");
}

} // namespace

int run_cancel_tests() {
    a_write_cancelled_before_it_starts_touches_nothing();
    a_write_cancelled_mid_sequence_removes_its_temp_file();
    a_cancelled_store_save_reports_cancelled_and_cleans_up();
    a_cancelled_read_reports_cancelled();
    a_cancelled_first_write_leaves_no_target();
    return g_failures;
}

} // namespace storage_test
