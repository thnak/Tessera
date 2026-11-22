// SPDX-License-Identifier: MIT
//
// The platform seam for the `shell` module (docs/specs/platform-shell-v1.md sections 2, 3, 5, 7 and
// 9.1; ADR-030; issue #450).
//
// This header is the *whole* platform boundary, exactly as `net_transport.hpp` is for the network
// module. A back end is one class that turns a request into a result; it names no `HWND`, no
// `IDropTarget`, no `IFileOpenDialog` and no platform type in its interface, so "no OS handles leak
// into UI/components" (section 7) is enforced by the type system rather than by review.
//
// **What the seam owns, and what the module owns.** This split is why the contract suite can pin
// behaviour once instead of per platform.
//
// | Rule (spec) | Owner | Why |
// |---|---|---|
// | section 2, bounded size | **module** (`kMaxShellClipboardBytes`) | one bound, one spelling, and
// the back end cannot be trusted to agree | | section 2, secure fields disable clipboard |
// **module** (the caller's `secure` flag) | it is a policy about the *field*, not about the OS | |
// section 2, never block the UI thread | back end + `IoTaskExecutor` | only the back end knows what
// block it would do | | section 5, cancellation is first-class | both | the module resolves
// `cancelled`; the back end observes the token and closes its dialog | | section 5, capability
// handles | **module** (`FileHandle::token`) | the token is the module's, so a back end cannot
// forge one | | section 8, path validation | **module** (`shell_path.cpp`) | one rule for every
// back end, and it is the security boundary | | section 9.1, one operation at a time per dialog |
// **module** (`Busy`) | it is a policy about the app, not about the OS | | section 9.1, capability
// negotiation | back end reports, module caches | the platform is the only thing that knows |
//
// **The contract every back end keeps.**
//
//   1. `start()` returns at once and never runs `on_done` on its own stack - not even for a failure
//      it already knows. The completion is posted through the injected `CompletionQueue`, so the
//      app's callback runs on the UI thread.
//   2. It observes the `CancellationToken`: a cancelled operation closes its dialog or releases its
//      clipboard lock and may report `cancelled` or nothing at all. Reporting nothing is legal
//      because the module has already resolved the operation.
//   3. It never validates a path. The module does that before the request reaches the back end, and
//      again on the way out, so a back end that returns a traversal path is caught.
//   4. It reports what the platform resolved, not what was asked: a dialog that the user cancelled
//      is `std::nullopt`, never an error.
//   5. It enforces no size bound of its own beyond what the platform requires; the module's bound
//   is
//      the normative one.
//
// `FakeShell` (`shell_fake.hpp`) is the in-tree implementation the contract suite runs against in
// normal CI, and the one a headless run uses.

#pragma once

#include "tessera/appfw/shell_types.hpp"
#include "tessera/runtime/async.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::appfw {

// What a back end can do and what it is currently allowed to do. The module caches this and
// re-queries it on a permission change (section 9.1).
struct ShellBackendCapabilities {
    ShellCapabilities capabilities;

    [[nodiscard]] friend constexpr bool operator==(const ShellBackendCapabilities&,
                                                   const ShellBackendCapabilities&) = default;
};

// One dialog request, in the three shapes section 5 defines. A single variant keeps the seam to one
// `start_dialog` entry point, so a back end implements one dispatch instead of three.
enum class DialogKind : std::uint8_t { Open = 0, Save, Folder };

struct DialogRequest {
    DialogKind kind = DialogKind::Open;
    OpenDialogSpec open;
    SaveDialogSpec save;
    FolderDialogSpec folder;
};

// What a dialog resolved to. `cancelled` is a value (section 5), so an empty `paths` with
// `cancelled == true` is a normal outcome and an empty `paths` with `cancelled == false` means the
// platform returned nothing for a request that was not cancelled.
struct DialogResult {
    std::vector<std::string> paths;
    bool cancelled = false;
};

// A drop the platform observed, already normalized: the payload, the local point, and the actions
// the source offered. The module decides whether a target accepts it.
struct DropRequest {
    DragPayload payload;
    float x = 0.0f;
    float y = 0.0f;
    DragActions offered = DragActions::Copy;
};

class ShellBackend {
public:
    // Exactly one call per accepted `start_*`, on the thread that drains the completion queue. It
    // takes the result by value: the back end has no further use for it.
    using ClipboardReadCompletion = std::function<void(std::optional<ClipboardContent>)>;
    using ClipboardWriteCompletion = std::function<void(bool)>;
    using DialogCompletion = std::function<void(DialogResult)>;
    using DropCompletion = std::function<void(DropAction)>;

    virtual ~ShellBackend() = default;

    ShellBackend(const ShellBackend&) = delete;
    ShellBackend& operator=(const ShellBackend&) = delete;

    // --- capability negotiation -------------------------------------------------------------

    // What this build can do. Cheap and side-effect free: the module calls it at start and on a
    // permission change, never per operation.
    [[nodiscard]] virtual ShellBackendCapabilities capabilities() const = 0;

    // A short, stable name for diagnostics ("fake", "win32"). Never a platform type and never
    // localised.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    // --- clipboard (section 2) --------------------------------------------------------------

    // Reads the clipboard. `std::nullopt` means "nothing on the clipboard", which is a clean empty
    // read and not a failure; a failure is reported through the module's error path.
    virtual bool start_clipboard_read(runtime::CancellationToken token,
                                      ClipboardReadCompletion on_done) = 0;
    virtual bool start_clipboard_write(ClipboardContent content, runtime::CancellationToken token,
                                       ClipboardWriteCompletion on_done) = 0;

    // --- dialogs (section 5) ----------------------------------------------------------------

    // Starts one dialog. Returns false when the back end cannot serve this request at all (no
    // capability, or already at its bound), and in that case `on_done` is never invoked.
    virtual bool start_dialog(DialogRequest request, runtime::CancellationToken token,
                              DialogCompletion on_done) = 0;

    // --- drag and drop (section 3) ----------------------------------------------------------

    // Starts one drop resolution. The module has already validated the payload's paths.
    virtual bool start_drop(DropRequest request, runtime::CancellationToken token,
                            DropCompletion on_done) = 0;

protected:
    ShellBackend() = default;
};

} // namespace tessera::appfw
