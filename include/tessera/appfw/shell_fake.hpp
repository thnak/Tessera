// SPDX-License-Identifier: MIT
//
// `FakeShell`: the deterministic in-tree back end (docs/specs/platform-shell-v1.md sections 2, 3, 5
// and 9; issue #450). It ships in the library rather than in `tests/unit` for the same reason
// `FakeNet` does: a headless run and a demo app use it to produce a defined result without an OS
// dialog, and the contract suite uses it as the reference behaviour every real back end is compared
// against.
//
// **Determinism, stated precisely.** A scripted operation carries a delay in milliseconds, and the
// delay is measured on the injected `Clock`, never on a wall clock and never with a sleep. The fake
// enqueues the operation and a test advances a `ManualClock`, calls `deliver_due()`, then drains
// the completion queue. Time is therefore an argument, and "the dialog is still open" is a fact the
// test asserts rather than waits for.
//
// **What it reproduces.** Section 9 lists the required cases; the fake makes each one reachable
// with data alone:
//
//   * a scripted clipboard read and write, including the empty clipboard (`std::nullopt`);
//   * a scripted dialog result: paths, or a cancel, or nothing at all;
//   * a scripted drop action, including a rejected drop (`DropAction::None`);
//   * a delay, so "still in flight" and the module's `Busy` rule are both expressible;
//   * a capability set that can be narrowed at runtime, so `Unsupported` and `Denied` are
//     distinguishable by construction;
//   * a refusal switch, so "the back end cannot serve this request" is reachable without a
//   platform.
//
// Every call it receives is recorded in `received()`, in order, so a test asserts the whole
// sequence
// - including the paths the module validated - instead of only the final result.

#pragma once

#include "tessera/appfw/shell_backend.hpp"
#include "tessera/appfw/shell_types.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/clock.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::appfw {

// What one scripted clipboard read reports.
struct FakeClipboardScript {
    std::optional<ClipboardContent> content;
    std::uint32_t delay_ms = 0;
};

// What one scripted dialog reports.
struct FakeDialogScript {
    DialogResult result;
    std::uint32_t delay_ms = 0;
};

// What one scripted drop reports.
struct FakeDropScript {
    DropAction action = DropAction::None;
    std::uint32_t delay_ms = 0;
};

// One recorded call, so a test can assert the sequence rather than infer it.
struct FakeShellCall {
    enum class Kind : std::uint8_t {
        ClipboardRead,
        ClipboardWrite,
        Dialog,
        Drop
    } kind = Kind::ClipboardRead;
    // The dialog kind, meaningful for `Dialog` only.
    DialogKind dialog_kind = DialogKind::Open;
    // The payload a write or a drop carried, already validated by the module.
    std::string text;
    // The paths a dialog request asked for, for a test that asserts the spec reached the back end.
    std::string default_directory;
};

// Every capability the spec's table grants a desktop platform. Exposed rather than hidden in the
// fake's implementation because a test that narrows the set usually wants to start from it, and
// because `ShellService` caches capabilities at construction (section 9.1: "query at startup and on
// permission changes"), so a test must set them *before* the service is built.
[[nodiscard]] ShellCapabilities desktop_shell_capabilities() noexcept;

class FakeShell final : public ShellBackend {
public:
    // The queue is where every completion is posted (contract 1: never inline) and the clock is
    // what a delay is measured on. Both are borrowed and must outlive the fake. `capabilities`
    // defaults to the desktop set, so a test that is not about negotiation does not have to set it
    // up.
    FakeShell(runtime::CompletionQueue& completions, runtime::Clock& clock,
              ShellCapabilities capabilities = desktop_shell_capabilities());

    FakeShell(const FakeShell&) = delete;
    FakeShell& operator=(const FakeShell&) = delete;

    // --- scripting -------------------------------------------------------------------------

    // Appends one scripted read. Reads are consumed in the order they were pushed; a read beyond
    // the script reports an empty clipboard, which is a clean result rather than a failure.
    void push_clipboard_read(std::optional<ClipboardContent> content, std::uint32_t delay_ms = 0);
    // The next write reports `ok`. A write beyond the script reports success, because a fake that
    // failed by default would make every test assert the failure path first.
    void push_clipboard_write(bool ok, std::uint32_t delay_ms = 0);
    // Appends one scripted dialog. A dialog beyond the script reports a cancel.
    void push_dialog(DialogResult result, std::uint32_t delay_ms = 0);
    void push_dialog_paths(std::vector<std::string> paths, std::uint32_t delay_ms = 0);
    void push_dialog_cancel(std::uint32_t delay_ms = 0);
    // Appends one scripted drop. A drop beyond the script is rejected (`DropAction::None`).
    void push_drop(DropAction action, std::uint32_t delay_ms = 0);

    // Section 9.1's capability negotiation: the fake reports what a test sets, so `Unsupported` and
    // `Denied` are reachable without a platform.
    void set_capabilities(ShellCapabilities capabilities) noexcept;
    [[nodiscard]] ShellBackendCapabilities capabilities() const override;

    // "The back end cannot serve this request": every `start_*` returns false and never completes.
    void set_available(bool available) noexcept;
    [[nodiscard]] bool available() const noexcept {
        return available_;
    }

    // --- driving ---------------------------------------------------------------------------

    // Delivers every operation whose due instant is at or before `clock().now_seconds()`, in call
    // order. Delivering means *posting* the completion, so the caller must then drain the
    // completion queue - the two stages stay separate, exactly as the real seam separates them.
    // Returns how many were delivered.
    std::size_t deliver_due();

    // Drops the script and every pending operation. A dropped operation never completes; the
    // module's own cancellation path is what resolves an operation whose back end went away.
    void clear();

    // --- observation -----------------------------------------------------------------------

    [[nodiscard]] const std::vector<FakeShellCall>& received() const noexcept {
        return received_;
    }
    // Operations still in flight (started and not yet due).
    [[nodiscard]] std::size_t in_flight() const noexcept {
        return pending_.size();
    }
    // Operations the fake observed as cancelled: one that arrived with an already-cancelled token,
    // plus one still outstanding whose token was cancelled after it started.
    [[nodiscard]] std::size_t cancelled_operations() const noexcept;
    [[nodiscard]] std::size_t scripted_reads() const noexcept {
        return read_script_.size();
    }
    [[nodiscard]] std::size_t scripted_dialogs() const noexcept {
        return dialog_script_.size();
    }

    [[nodiscard]] std::string_view name() const noexcept override;

    // `ShellBackend`.
    bool start_clipboard_read(runtime::CancellationToken token,
                              ClipboardReadCompletion on_done) override;
    bool start_clipboard_write(ClipboardContent content, runtime::CancellationToken token,
                               ClipboardWriteCompletion on_done) override;
    bool start_dialog(DialogRequest request, runtime::CancellationToken token,
                      DialogCompletion on_done) override;
    bool start_drop(DropRequest request, runtime::CancellationToken token,
                    DropCompletion on_done) override;

private:
    // One pending operation. The three completion shapes are kept apart rather than unified behind
    // a variant, because each one's result type is different and a variant would only move the
    // switch.
    struct Pending {
        FakeShellCall::Kind kind = FakeShellCall::Kind::ClipboardRead;
        runtime::CancellationToken token;
        double due_seconds = 0.0;
        std::optional<ClipboardContent> read_content;
        bool write_ok = true;
        DialogResult dialog;
        DropAction drop = DropAction::None;
        ClipboardReadCompletion on_read;
        ClipboardWriteCompletion on_write;
        DialogCompletion on_dialog;
        DropCompletion on_drop;
    };

    [[nodiscard]] bool token_cancelled(const runtime::CancellationToken& token) const;

    runtime::CompletionQueue* completions_ = nullptr;
    runtime::Clock* clock_ = nullptr;
    ShellCapabilities capabilities_;

    std::vector<FakeClipboardScript> read_script_;
    std::vector<std::pair<bool, std::uint32_t>> write_script_;
    std::vector<FakeDialogScript> dialog_script_;
    std::vector<FakeDropScript> drop_script_;
    std::size_t read_cursor_ = 0;
    std::size_t write_cursor_ = 0;
    std::size_t dialog_cursor_ = 0;
    std::size_t drop_cursor_ = 0;

    std::vector<Pending> pending_;
    std::vector<FakeShellCall> received_;
    std::size_t cancelled_operations_ = 0;
    bool available_ = true;
};

} // namespace tessera::appfw
