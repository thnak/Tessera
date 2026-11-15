// SPDX-License-Identifier: MIT
//
// AF7 (issue #179): the executor seam.
//
// Three things are proved here:
//
//   1. **The manual executor drives the whole module deterministically.** Submit does nothing until
//      the test pumps, the body runs before the completion, and the completion is where the result
//      arrives. That is the round trip, with no thread and no sleep.
//   2. **The production executor runs the same work off the calling thread.** This is the *only*
//      test in the suite that uses a real thread, and it exists to prove the seam rather than to
//      exercise the module: it asserts the work body did not run on the thread that submitted it
//      and that the completion was delivered through the completion queue. Everything else is
//      manual.
//   3. **The real file system's replace semantics are what the design claims.** Windows is the lane
//      where "just rename" is wrong, so the atomic replace is run against a real file under the
//      build tree (never `%TEMP%`, which the CI lanes have ACL trouble with) and asserts
//      replace-over-existing and old-content-survives-failure behaviour.

#include "appfw_storage_test_support.hpp"
#include "tessera/hal/filesystem.hpp"
#include "tessera/runtime/worker_io_executor.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace storage_test {
namespace {

using tessera::appfw::Settings;
using tessera::runtime::CompletionQueue;
using tessera::runtime::WorkerIoExecutor;

// One submission, three observations: post does not run the body, the body runs when pumped, and
// the completion runs after it with the result.
void the_manual_executor_round_trips_a_submission() {
    Harness harness;
    // Seed through `save`, so the stored bytes are a real envelope: this test is about the executor
    // round trip, not about the format.
    harness.store.save("doc", "value", {}, {});
    harness.run_one();
    expect(harness.files.has_file("/store/doc"), "the seeded entry is on disk");
    harness.files.calls.clear();

    bool invoked = false;
    std::expected<tessera::appfw::StoredValue, StorageError> loaded;
    const bool posted = harness.store.load("doc", {}, [&](auto value) {
        invoked = true;
        loaded = std::move(value);
    });

    expect(posted, "the load is posted");
    expect(!invoked, "posting does not run the completion");
    expect(harness.executor.pending_work() == 1, "one body is queued");

    // The body runs first and only then does the completion become runnable: the two stages the
    // real executor has. A completion cannot observe a result the body has not produced.
    expect(harness.executor.run_completions(4) == 0, "no completion exists before the body ran");
    expect(!invoked, "the completion still has not run");

    expect(harness.executor.run(1) == 1, "the body ran");
    expect(harness.executor.pending_completions() == 1, "its completion is now queued");
    expect(!invoked, "the body does not invoke the completion inline");

    expect(harness.executor.run_completions(1) == 1, "the completion ran");
    expect(invoked, "the completion was invoked");
    expect(loaded.has_value(), "the completion received a value");
    expect(loaded->data == "value", "the payload round trips through the executor");
}

// The executor's bound is the module's bound: a submission past it is refused (no completion, the
// caller sees `false`) rather than queued without limit.
void the_manual_executor_honours_its_capacity() {
    ManualIoExecutor executor(2);
    FakeFileSystem files;
    StorageConfig config;
    config.root = "/store";
    Storage store(config, executor, files);

    int completions = 0;
    expect(store.save("a", "1", {}, [&](auto) { ++completions; }), "first save accepted");
    expect(store.save("b", "2", {}, [&](auto) { ++completions; }), "second save accepted");
    expect(!store.save("c", "3", {}, [&](auto) { ++completions; }),
           "third save refused at the bound");
    expect(completions == 0, "a refused submission never invokes its completion");

    executor.run(8);
    executor.run_completions(8);
    expect(completions == 2, "the two accepted saves completed");
    expect(executor.stats().rejected == 1, "the refusal is counted");
}

// A work body that throws is counted and does not take the executor down; the completion still runs
// (it carries the store's error value, not an exception).
void a_throwing_body_does_not_escape_the_executor() {
    ManualIoExecutor executor(4);
    bool completed = false;
    const bool posted = executor.post(tessera::runtime::IoTask{
        [] { throw std::runtime_error("body exploded"); },
        [&] { completed = true; },
    });
    expect(posted, "the throwing body is accepted");
    executor.run(1);
    executor.run_completions(1);
    expect(executor.stats().failed == 1, "the throw is counted as a failure");
    expect(completed, "the completion still ran");
}

// The production seam: work runs on a worker thread, not on the submitting thread, and the
// completion arrives through the completion queue. This is the only place in the suite that uses a
// real thread; it proves the seam, not the module (every module test above is manual).
void the_worker_executor_runs_work_off_the_submitting_thread() {
    CompletionQueue queue(64);
    WorkerIoExecutor executor(queue, WorkerIoExecutor::Config{2, 2, 64});

    const std::thread::id submitting = std::this_thread::get_id();
    std::promise<void> body_ran;
    auto body_future = body_ran.get_future();
    std::atomic<bool> ran_off_thread{false};
    std::atomic<bool> completion_on_drain{false};

    const bool posted = executor.post(tessera::runtime::IoTask{
        [&] {
            ran_off_thread = std::this_thread::get_id() != submitting;
            body_ran.set_value();
        },
        [&] { completion_on_drain = true; },
    });
    expect(posted, "the production executor accepts the task");
    expect(!ran_off_thread.load(), "posting does not run the body inline");
    expect(executor.stats().in_flight == 1, "one task is in flight before it runs");

    // Wait for the body, then drain the queue for its completion. Both waits are bounded by a hang
    // guard rather than by a fixed sleep: in practice the body finishes immediately and the
    // completion is on the queue by the first drain.
    expect(body_future.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
           "the body ran");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!completion_on_drain.load() && std::chrono::steady_clock::now() < deadline) {
        queue.run(8);
        std::this_thread::yield();
    }

    expect(completion_on_drain.load(), "the completion was delivered through the queue");
    expect(ran_off_thread.load(), "the work body ran on a worker thread, not the caller's");
    expect(executor.stats().completed == 1, "the production executor counted the completion");
    expect(executor.stats().in_flight == 0, "the slot was released");
    executor.shutdown();
}

// Settings round trip through the same async path, including the ordering of the pairs.
void settings_round_trip_through_the_executor() {
    Harness harness;
    const Settings written{{"theme", "dark"}, {"locale", "en-GB"}, {"scale", "1.5"}};
    bool saved = false;
    harness.store.save_settings("prefs", written, {},
                                [&](auto status) { saved = status.has_value(); });
    harness.run_one();
    expect(saved, "the settings were saved");

    std::expected<Settings, StorageError> loaded;
    harness.store.load_settings("prefs", {}, [&](auto value) { loaded = std::move(value); });
    harness.run_all();
    expect(loaded.has_value(), "the settings load back");
    expect(loaded->size() == 3, "every pair round trips");
    expect((*loaded)[0] == std::make_pair(std::string("theme"), std::string("dark")),
           "order is preserved");
    expect((*loaded)[2] == std::make_pair(std::string("scale"), std::string("1.5")),
           "the last pair round trips");
}

// The real file system, under the build tree. This is the test that validates the platform's
// replace semantics: on Windows a plain rename would fail here because the target exists.
void the_real_file_system_replaces_an_existing_target() {
    namespace fs = std::filesystem;
    const fs::path root = fs::path(TESSERA_STORAGE_SCRATCH_DIR) / "atomic";
    std::error_code code;
    fs::remove_all(root, code);
    fs::create_directories(root, code);
    expect(!code, "the scratch directory was created under the build tree");

    auto files = tessera::hal::create_native_file_system();
    const std::string target = (root / "doc").string();
    expect(files->write(target, "first content").has_value(), "the first write landed");
    expect(files->read(target, 1024).value_or("") == "first content",
           "the first content reads back");

    // The exact sequence `atomic_replace` uses, against the real OS.
    const std::string temp = target + ".0000000000000001.tmp";
    expect(files->write(temp, "second content").has_value(), "the temp write landed");
    expect(files->flush(temp).has_value(), "the temp flush succeeded");
    const auto replaced = files->replace(temp, target);
    expect(replaced.has_value(), "replace over an existing target succeeded");
    expect(files->read(target, 1024).value_or("") == "second content",
           "the target holds the new content");
    expect(!files->exists(temp), "the temp file no longer exists");

    // And the failure path: a replace whose source is missing leaves the target alone.
    const auto missing = files->replace(target + ".missing.tmp", target);
    expect(!missing.has_value(), "a replace with a missing source fails");
    expect(files->read(target, 1024).value_or("") == "second content",
           "the target survived the failed replace");

    fs::remove_all(root, code);
}

} // namespace

int run_executor_tests() {
    the_manual_executor_round_trips_a_submission();
    the_manual_executor_honours_its_capacity();
    a_throwing_body_does_not_escape_the_executor();
    settings_round_trip_through_the_executor();
    the_real_file_system_replaces_an_existing_target();
    the_worker_executor_runs_work_off_the_submitting_thread();
    return g_failures;
}

} // namespace storage_test
