// SPDX-License-Identifier: MIT
//
// `ShellService`: the app-facing shell (docs/specs/platform-shell-v1.md sections 2, 3, 5, 7, 8 and
// 9.1; ADR-030; issue #450).
//
// The service is the *policy* half of the module and the back end (`shell_backend.hpp`) is the
// *platform* half. Everything that must be true on every platform lives here, once:
//
//   * **Capability negotiation** (section 9.1). `capabilities()` is cached from the back end at
//     construction and refreshed by `refresh_capabilities()`, which a permission change calls. An
//     operation whose capability is absent returns `Unsupported`; one whose capability is present
//     but not permitted returns `Denied`. Neither is a silent no-op (gap G-15/Q-14).
//   * **Bounds** (section 2). A clipboard payload over `kMaxShellClipboardBytes` is `TooLarge`
//     before it reaches the back end, and a read that comes back over the bound is refused rather
//     than truncated.
//   * **Secure fields** (section 2). `read_clipboard(secure = true)` is refused with `Denied`, so a
//     password field cannot be pasted into by accident and the policy is one line in one place.
//   * **One operation at a time per kind** (section 9.1). A second dialog while one is open is
//     `Busy`, which is the conservative reading of "whether a second request is queued or rejected
//     is not specified here".
//   * **Capability handles** (section 5, 9.1). Every `FileHandle` carries a token this service
//     issued; `validate()` checks it, so a handle cannot be forged or replayed.
//   * **Path validation** (section 8). Every path a back end returns is checked by
//     `shell_path_check` before it becomes a `FileHandle`, and every path a drop carries is checked
//     before the back end sees it.
//   * **Diagnostics** (ADR-022). A bounded ring plus counters, with redacted detail strings.
//
// **Async shape.** Section 7 says every operation is an async task on an `io`/platform executor
// with its result on the UI thread. The seam's `start_*` methods already have that shape (they
// return at once and post their completion through the injected `CompletionQueue`), so this service
// exposes the coroutine-free `submit_*` form plus a `runtime::Task` wrapper for a caller that is
// itself a coroutine. The task's own token is deliberately empty, exactly as `NetClient::send`
// documents: cancelling it would make the runtime throw `TaskCancelled` at the `co_await`, and
// section 5 wants `cancelled` **as a value**.
//
// **Not in this slice**, named in docs/design/shell-module.md section 7: clipboard files/URIs,
// cross-process drag promises, the menu bar and tray models, global shortcuts, and every
// non-Windows back end.

#pragma once

#include "tessera/appfw/shell_backend.hpp"
#include "tessera/appfw/shell_types.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/task.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::appfw {

namespace detail {
class ShellServiceImpl;
} // namespace detail

// Everything the service is configured with, all of it explicit (services/common-contract.md
// section 1: "Configuration is explicit").
struct ShellConfig {
    // Section 9.1's "capped at 64 MiB/16 files (configurable)".
    std::size_t max_drop_files = kMaxShellDropFiles;
    std::uint64_t max_drop_bytes = kMaxShellDropBytes;
    // How many failures the diagnostics ring keeps. 0 disables the ring but keeps the counters.
    std::size_t diagnostic_capacity = 16;
    // Section 2: "Secure fields disable clipboard by default". A caller that wants the opposite
    // (a password manager's own paste button) sets this, and the reason is then explicit.
    bool allow_secure_clipboard = false;
};

// One per app (or per window) scope. Borrowed dependencies, every one of them explicit:
//
//   * `backend` - the platform half (`FakeShell` in tests and headless runs);
//   * `completions` - the queue every completion runs on: the UI thread.
//
// Both must outlive the service. Non-movable: the impl is shared with in-flight operations so a
// destroyed service cannot dangle a callback into freed state.
class ShellService {
public:
    // What a clipboard read delivers. `std::nullopt` is "nothing on the clipboard", a clean empty
    // read; a failure is the `expected`'s error.
    using ClipboardCallback =
        std::function<void(std::expected<std::optional<ClipboardContent>, ShellError>)>;
    using WriteCallback = std::function<void(std::expected<void, ShellError>)>;
    using DialogCallback = std::function<void(std::expected<std::vector<FileHandle>, ShellError>)>;
    using DropCallback = std::function<void(std::expected<DropAction, ShellError>)>;

    ShellService(ShellConfig config, ShellBackend& backend, runtime::CompletionQueue& completions);
    ~ShellService();

    ShellService(const ShellService&) = delete;
    ShellService& operator=(const ShellService&) = delete;
    ShellService(ShellService&&) = delete;
    ShellService& operator=(ShellService&&) = delete;

    // --- capabilities (section 9.1) ---------------------------------------------------------

    [[nodiscard]] ShellCapabilities capabilities() const;
    // Re-queries the back end. A permission change calls this; nothing else does.
    void refresh_capabilities();
    // Section 9.1's "Unsupported capability returns Error::Unsupported": the check a caller can
    // make before offering an action, so a component hides it instead of failing on use.
    [[nodiscard]] std::expected<void, ShellError> require(ShellCapability capability) const;

    // --- clipboard (section 2) --------------------------------------------------------------

    // `secure` is the field's own flag: a secure field's read is refused unless the config allows
    // it. Returns false when the operation was refused before anything started, in which case
    // `on_done` is never invoked and the error is the return value.
    [[nodiscard]] std::expected<void, ShellError> submit_clipboard_read(bool secure,
                                                                        ClipboardCallback on_done);
    [[nodiscard]] std::expected<void, ShellError> submit_clipboard_write(ClipboardContent content,
                                                                         WriteCallback on_done);

    // The coroutine form, for a caller that is itself a coroutine. The task's own token is empty on
    // purpose (see the header comment).
    [[nodiscard]] runtime::Task<std::expected<std::optional<ClipboardContent>, ShellError>>
    read_clipboard(bool secure = false);
    [[nodiscard]] runtime::Task<std::expected<void, ShellError>>
    write_clipboard(ClipboardContent content);

    // --- external URLs ----------------------------------------------------------------------

    // Hands `url` to the system handler (`ShellCapability::OpenUrl`). Refused before anything
    // starts when the capability is missing, the URL is empty or longer than `kMaxShellUrlBytes`,
    // or the back end has no handler; in that case `on_done` is never invoked. Which schemes a
    // caller may open is its own policy (`LinkRouter`), not the service's.
    [[nodiscard]] std::expected<void, ShellError> submit_open_url(std::string url,
                                                                  WriteCallback on_done);

    // --- dialogs (section 5) ----------------------------------------------------------------

    [[nodiscard]] std::expected<void, ShellError> submit_open_dialog(OpenDialogSpec spec,
                                                                     DialogCallback on_done);
    [[nodiscard]] std::expected<void, ShellError> submit_save_dialog(SaveDialogSpec spec,
                                                                     DialogCallback on_done);
    [[nodiscard]] std::expected<void, ShellError> submit_folder_dialog(FolderDialogSpec spec,
                                                                       DialogCallback on_done);

    [[nodiscard]] runtime::Task<std::expected<std::vector<FileHandle>, ShellError>>
    open_file(OpenDialogSpec spec);
    [[nodiscard]] runtime::Task<std::expected<std::vector<FileHandle>, ShellError>>
    save_file(SaveDialogSpec spec);
    [[nodiscard]] runtime::Task<std::expected<std::vector<FileHandle>, ShellError>>
    pick_folder(FolderDialogSpec spec);

    // Section 5's "cancellation is first-class". Cancels the in-flight operation of that kind,
    // which resolves it as `Cancelled` exactly once. False when nothing of that kind is in flight.
    bool cancel_dialog();
    bool cancel_clipboard();

    // --- drag and drop (section 3) ----------------------------------------------------------

    // Resolves one drop. The payload's paths are validated here before the back end sees them, and
    // the file count/byte budget is enforced here (section 9.1).
    [[nodiscard]] std::expected<void, ShellError> submit_drop(DropRequest request,
                                                              DropCallback on_done);

    // --- capability handles (section 5, 9.1) ------------------------------------------------

    // True when `handle` was issued by this service and has not been revoked.
    [[nodiscard]] bool validate(const FileHandle& handle) const;
    // Revokes one handle. False for an unknown or already-revoked token.
    bool revoke(const FileHandle& handle);
    [[nodiscard]] std::size_t live_handles() const noexcept;

    // --- observation ------------------------------------------------------------------------

    [[nodiscard]] const ShellConfig& config() const noexcept;
    // Non-copyable on purpose: a diagnostics snapshot builds a fresh ring and copies the counters,
    // so it must be assigned to a variable, not returned through a reference.
    [[nodiscard]] ShellDiagnostics diagnostics() const;
    [[nodiscard]] std::string_view backend_name() const noexcept;
    [[nodiscard]] std::size_t in_flight() const noexcept;

private:
    // The coroutine bodies, context-first because `runtime::Task`'s promise consumes the context
    // there (docs/design/services.md section 1.1).
    static runtime::Task<std::expected<std::optional<ClipboardContent>, ShellError>>
    read_body(runtime::TaskContext context, ShellService& self, bool secure);
    static runtime::Task<std::expected<void, ShellError>>
    write_body(runtime::TaskContext context, ShellService& self, ClipboardContent content);
    static runtime::Task<std::expected<std::vector<FileHandle>, ShellError>>
    dialog_body(runtime::TaskContext context, ShellService& self, DialogRequest request);

    std::shared_ptr<detail::ShellServiceImpl> impl_;
};

} // namespace tessera::appfw
