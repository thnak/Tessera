// SPDX-License-Identifier: MIT
//
// `ShellServiceImpl`: the state behind `ShellService` (issue #450). It is a separate class so the
// public header carries no implementation detail and so an in-flight operation can hold a
// `shared_ptr` to it: a service destroyed while a dialog is open must not leave a callback pointing
// at freed state, and the `weak_ptr` check in every completion is what makes that true.

#pragma once

#include "tessera/appfw/shell.hpp"
#include "tessera/appfw/shell_backend.hpp"
#include "tessera/appfw/shell_types.hpp"
#include "tessera/runtime/async.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::appfw::detail {

// The redaction rule (section 8: "Redact clipboard/path contents in logs/snapshots"). A detail
// string never carries a payload or a full path: it carries the operation and, at most, the reason.
// This is the one place that decides what a diagnostic may say, and it is shared by both halves of
// the impl so a detail built in one file cannot be spelled differently in the other.
[[nodiscard]] std::string redacted_detail(std::string_view operation, std::string_view reason);

// One issued capability handle. `token` is the value the caller holds; the entry is what makes it
// valid, so revoking is a removal and a forged token simply is not found.
struct HandleEntry {
    std::uint64_t token = 0;
    std::string path;
};

// Which operation kind is in flight, for the one-at-a-time rule (section 9.1).
enum class OperationKind : std::uint8_t { None = 0, ClipboardRead, ClipboardWrite, Dialog, Drop };

class ShellServiceImpl final : public std::enable_shared_from_this<ShellServiceImpl> {
public:
    ShellServiceImpl(ShellConfig config, ShellBackend& backend,
                     runtime::CompletionQueue& completions);
    ~ShellServiceImpl();

    ShellServiceImpl(const ShellServiceImpl&) = delete;
    ShellServiceImpl& operator=(const ShellServiceImpl&) = delete;

    // --- capability -------------------------------------------------------------------------

    [[nodiscard]] ShellCapabilities capabilities() const noexcept {
        return capabilities_;
    }
    void refresh_capabilities();
    [[nodiscard]] std::expected<void, ShellError> require(ShellCapability capability) const;

    // --- clipboard --------------------------------------------------------------------------

    [[nodiscard]] std::expected<void, ShellError>
    submit_clipboard_read(bool secure, ShellService::ClipboardCallback on_done);
    [[nodiscard]] std::expected<void, ShellError>
    submit_clipboard_write(ClipboardContent content, ShellService::WriteCallback on_done);
    bool cancel_clipboard();

    // --- dialogs ----------------------------------------------------------------------------

    [[nodiscard]] std::expected<void, ShellError>
    submit_dialog(DialogRequest request, ShellService::DialogCallback on_done);
    bool cancel_dialog();
    bool cancel_drop();

    // --- drag and drop ----------------------------------------------------------------------

    [[nodiscard]] std::expected<void, ShellError> submit_drop(DropRequest request,
                                                              ShellService::DropCallback on_done);

    // --- handles ----------------------------------------------------------------------------

    [[nodiscard]] bool validate(const FileHandle& handle) const;
    bool revoke(const FileHandle& handle);
    [[nodiscard]] std::size_t live_handles() const noexcept {
        return handles_.size();
    }

    // --- observation ------------------------------------------------------------------------

    [[nodiscard]] const ShellConfig& config() const noexcept {
        return config_;
    }
    [[nodiscard]] ShellDiagnostics diagnostics() const;
    [[nodiscard]] std::string_view backend_name() const noexcept;
    [[nodiscard]] std::size_t in_flight() const noexcept {
        return in_flight_;
    }

    // --- shared helpers used by the coroutine bodies ----------------------------------------

    // Issues a handle for a validated path. Returns an invalid handle when the path is refused, so
    // the caller reports the rejection rather than handing out a token.
    [[nodiscard]] std::expected<FileHandle, ShellError> issue_handle(std::string path);

    // Records one failure in the bounded ring and bumps the matching counter.
    void record(ShellErrorKind kind, std::string detail);

    // The one-at-a-time gate. Returns `Busy` when an operation of the same kind is already running.
    [[nodiscard]] std::expected<void, ShellError> begin(OperationKind kind);
    void end(OperationKind kind);

    // Resolves an in-flight operation as cancelled, exactly once.
    void resolve_cancelled(OperationKind kind);

    [[nodiscard]] runtime::CompletionQueue& completions() noexcept {
        return *completions_;
    }

private:
    // Posts `callback` through the completion queue, dropping it if the queue is full (a bounded
    // queue is a bound, and the module's own cancellation path is what resolves the operation).
    void post(std::function<void()> callback);

    ShellConfig config_;
    ShellBackend* backend_ = nullptr;
    runtime::CompletionQueue* completions_ = nullptr;
    ShellCapabilities capabilities_;

    // The in-flight operation per kind, so a second one is `Busy` and a cancel can find it.
    runtime::CancellationSource clipboard_cancel_;
    runtime::CancellationSource dialog_cancel_;
    runtime::CancellationSource drop_cancel_;
    bool clipboard_in_flight_ = false;
    bool dialog_in_flight_ = false;
    bool drop_in_flight_ = false;
    // Set by the first cancel of the current operation and cleared when it resolves, so a second
    // cancel is a no-op rather than a second cancellation of an already-cancelled source.
    bool clipboard_cancelled_ = false;
    bool dialog_cancelled_ = false;
    bool drop_cancelled_ = false;
    std::size_t in_flight_ = 0;

    std::vector<HandleEntry> handles_;
    std::uint64_t next_token_ = 1;

    // The bounded diagnostics ring (ADR-022).
    std::deque<ShellDiagnostic> ring_;
    std::size_t recorded_ = 0;
    std::size_t dropped_ = 0;
    ShellDiagnostics counters_;
};

} // namespace tessera::appfw::detail
