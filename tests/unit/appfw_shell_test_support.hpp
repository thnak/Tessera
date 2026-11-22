// SPDX-License-Identifier: MIT
//
// Fixtures for the `shell` contract suite (docs/specs/platform-shell-v1.md section 9; issue #450).
//
// Determinism is structural, not a promise: time is a `ManualClock`, the platform is `FakeShell`,
// and the delivery stage is a `CompletionQueue` the test drains. No test in this suite opens a
// dialog, touches a real clipboard, reads a wall clock, sleeps, starts a thread or touches the file
// system - and because every stage is a hand-driven FIFO, a test asserts *what happened at each
// stage* instead of waiting for it.
//
// The pump deliberately separates the stages the way a frame does:
//
//   back-end delivery -> completions
//
// which is `RuntimeLoop::tick()`'s order (drain completions, then fire timers), so a bug that only
// appears when the order changes is visible here.

#pragma once

#include "tessera/appfw/shell.hpp"
#include "tessera/appfw/shell_backend.hpp"
#include "tessera/appfw/shell_fake.hpp"
#include "tessera/appfw/shell_path.hpp"
#include "tessera/appfw/shell_types.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/task.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shell_test {

namespace appfw = tessera::appfw;
namespace runtime = tessera::runtime;

using appfw::ClipboardContent;
using appfw::DialogFilter;
using appfw::DialogKind;
using appfw::DialogRequest;
using appfw::DialogResult;
using appfw::DragActions;
using appfw::DragPayload;
using appfw::DragPayloadKind;
using appfw::DropAction;
using appfw::DropRequest;
using appfw::FakeShell;
using appfw::FileHandle;
using appfw::FolderDialogSpec;
using appfw::OpenDialogSpec;
using appfw::PathRejection;
using appfw::SaveDialogSpec;
using appfw::ShellCapabilities;
using appfw::ShellCapability;
using appfw::ShellConfig;
using appfw::ShellDiagnostics;
using appfw::ShellError;
using appfw::ShellErrorKind;
using appfw::ShellService;

inline int g_failures = 0;

inline bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "appfw_shell: " << message << "\n";
        ++g_failures;
    }
    return condition;
}

// The whole deterministic stack. Declaration order is dependency order: the clock and the queue are
// destroyed last because the service and the fake both borrow them.
//
// `capabilities` is a constructor argument rather than something a test sets afterwards, because
// `ShellService` caches the set at construction (section 9.1: "query at startup and on permission
// changes"). A test that narrows the set passes it here; a test that changes it at runtime calls
// `refresh_capabilities()`.
struct Harness {
    explicit Harness(ShellConfig cfg = {},
                     ShellCapabilities capabilities = appfw::desktop_shell_capabilities())
        : config(std::move(cfg)),
          fake(completions, clock, capabilities),
          service(config, fake, completions) {}

    runtime::ManualClock clock;
    runtime::CompletionQueue completions{256};
    ShellConfig config;
    FakeShell fake;
    ShellService service;
};

// Runs every stage until nothing is ready. Returns how many rounds made progress, which is what a
// test asserts when it needs to prove an operation *stopped* making progress.
inline std::size_t pump(Harness& harness, std::size_t budget = 64) {
    std::size_t rounds = 0;
    for (std::size_t index = 0; index < budget; ++index) {
        bool progress = false;
        progress = harness.fake.deliver_due() > 0 || progress;
        progress = harness.completions.run(64) > 0 || progress;
        if (!progress) {
            return rounds;
        }
        ++rounds;
    }
    return rounds;
}

inline std::size_t advance(Harness& harness, double seconds, std::size_t budget = 64) {
    harness.clock.advance(seconds);
    return pump(harness, budget);
}

// A path that passes `shell_path_check` on both lanes, so a fixture is not lane-specific.
inline std::string valid_path(std::string_view name) {
    return std::string("/tmp/tessera-shell/") + std::string(name);
}

inline OpenDialogSpec open_spec() {
    OpenDialogSpec spec;
    spec.title = "Open";
    spec.filters.push_back(DialogFilter{"Text", {".txt", ".md"}});
    return spec;
}

inline DragPayload file_payload(std::string path) {
    DragPayload payload;
    payload.kind = DragPayloadKind::FilePromise;
    payload.text = std::move(path);
    return payload;
}

inline DropRequest drop_request(DragPayload payload) {
    DropRequest request;
    request.payload = std::move(payload);
    request.offered = DragActions::Copy;
    return request;
}

// Drives a coroutine to completion and hands its value back through `out`.
//
// A `runtime::Task<T>`'s value is readable only by awaiting it, so the suite cannot call
// `read_clipboard`, pump, and then inspect the task - it has to await from inside another
// coroutine. This is that coroutine: context-first, because `Task`'s promise consumes the context
// there (docs/design/services.md section 1.1), and with the result written into a caller-owned slot
// so the assertion reads a plain value. It exercises the real path, including the awaiter's
// suspension and its queue-driven resume.
template<typename T>
runtime::Task<void> await_task(runtime::TaskContext, runtime::Task<T> task, T& out) {
    out = co_await std::move(task);
    co_return;
}

// Starts `await_task` and pumps until it is done (or the budget runs out). Returns whether the task
// finished without an exception.
template<typename T>
bool run_task(Harness& harness, runtime::Task<T> task, T& out, int budget = 64) {
    runtime::TaskContext context;
    context.queue = &harness.completions;
    auto driver = await_task<T>(context, std::move(task), out);
    if (!driver.start()) {
        return false;
    }
    for (int round = 0; round < budget && !driver.done(); ++round) {
        pump(harness);
    }
    return driver.done() && driver.exception() == nullptr;
}

} // namespace shell_test
