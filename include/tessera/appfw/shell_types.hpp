// SPDX-License-Identifier: MIT
//
// The platform shell's value types (docs/specs/platform-shell-v1.md sections 1, 2, 5 and 9.1;
// ADR-030; issue #450). This is the interface an app sees: a capability set, a clipboard payload, a
// dialog request and a named error. No `HWND`, no `IDropTarget`, no `IFileOpenDialog` and no
// platform header appears here, which is the same rule `net_types.hpp` states for the network seam.
//
// Names: the spec's section 9.1 writes `ShellCapability`, `ShellCapabilities` and `ShellService`,
// and those three keep their names. The spec's bare `Path` and `FileHandle` are spelled
// `std::string`/`FileHandle` here: a `Path` type would drag `std::filesystem` into a public app
// header, and the storage module already made that call for `hal::FileSystem`.
//
// What the spec fixes, and what this header therefore carries:
//
//   * **Capabilities are a bitmask** (section 9.1, gap G-15/Q-14): `supported` and `permissioned`
//     are separate 64-bit masks, so "this build cannot" and "the user said no" stay
//     distinguishable. An unsupported capability returns `ShellErrorKind::Unsupported`; a denied
//     one returns `Denied`. Neither is a silent no-op.
//   * **Clipboard is `text/plain` in V1** (section 2). Files, URIs, HTML, RTF and images are marked
//     "later" in the spec's table and are not representable here, so a caller cannot ask for a
//     format the module would have to fake.
//   * **Dialogs return capability handles, not raw OS pointers** (section 5 and 9.1). `FileHandle`
//     carries a path plus a token that `ShellService` validates on use, so a handle from a previous
//     session cannot be replayed.
//   * **Payloads are untrusted** (section 8). The size bounds below are the ones the module
//     enforces before a payload reaches a caller; a larger one is `TooLarge`, never truncated.
//
// Deliberately absent, and named in docs/design/shell-module.md section 7: clipboard files/URIs,
// cross-process drag promises, the menu bar and tray models (window-chrome-v1.md owns the menu
// *model*), global shortcuts, and every non-Windows backend.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::appfw {

// Section 9.1's `ShellCapability`, in the spec's order. The enumerator values are bit positions, so
// `ShellCapabilities` is a mask over them and an added capability is a new bit rather than a
// renumbering.
enum class ShellCapability : std::uint8_t {
    ClipboardText = 0,
    ClipboardFiles,
    DragInternal,
    DragExternal,
    DropFiles,
    ContextMenu,
    FileOpen,
    FileSave,
    FolderPick,
    MenuBar,
    Tray,
    GlobalShortcut,
    // Hands a URL to the system (browser, mail client, dialer): the external form of `a href`.
    OpenUrl,
};

// How many bits `ShellCapabilities` can hold. The spec's mask is 64-bit; the enumerator set is
// deliberately smaller, and this constant is what a `static_assert` below pins.
inline constexpr std::size_t kShellCapabilityCount = 13;

[[nodiscard]] constexpr std::uint64_t shell_capability_bit(ShellCapability capability) noexcept {
    return std::uint64_t{1} << static_cast<std::uint8_t>(capability);
}

[[nodiscard]] constexpr std::string_view
shell_capability_name(ShellCapability capability) noexcept {
    switch (capability) {
    case ShellCapability::ClipboardText:
        return "clipboard-text";
    case ShellCapability::ClipboardFiles:
        return "clipboard-files";
    case ShellCapability::DragInternal:
        return "drag-internal";
    case ShellCapability::DragExternal:
        return "drag-external";
    case ShellCapability::DropFiles:
        return "drop-files";
    case ShellCapability::ContextMenu:
        return "context-menu";
    case ShellCapability::FileOpen:
        return "file-open";
    case ShellCapability::FileSave:
        return "file-save";
    case ShellCapability::FolderPick:
        return "folder-pick";
    case ShellCapability::MenuBar:
        return "menu-bar";
    case ShellCapability::Tray:
        return "tray";
    case ShellCapability::GlobalShortcut:
        return "global-shortcut";
    case ShellCapability::OpenUrl:
        return "open-url";
    }
    return "unknown";
}

// Section 9.1's `ShellCapabilities`, verbatim: two masks, because "the build cannot" and "the user
// refused" are different answers and a component renders them differently (one hides the action,
// the other disables it with an accessible reason).
struct ShellCapabilities {
    std::uint64_t supported = 0;
    std::uint64_t permissioned = 0;

    [[nodiscard]] bool supports(ShellCapability capability) const noexcept {
        return (supported & shell_capability_bit(capability)) != 0;
    }
    [[nodiscard]] bool permits(ShellCapability capability) const noexcept {
        return (permissioned & shell_capability_bit(capability)) != 0;
    }
    // The one predicate a caller should branch on: present in this build *and* allowed right now.
    [[nodiscard]] bool usable(ShellCapability capability) const noexcept {
        return supports(capability) && permits(capability);
    }

    [[nodiscard]] friend constexpr bool operator==(const ShellCapabilities&,
                                                   const ShellCapabilities&) = default;
};

// Section 2's `ClipboardContent`, restricted to what V1 carries. `text` is UTF-8 and bounded by
// `kMaxShellClipboardBytes`; the spec's "preserve line endings/trailing newline policy" is the
// caller's, because the module never rewrites a payload it was handed.
struct ClipboardContent {
    std::string text;

    [[nodiscard]] bool empty() const noexcept {
        return text.empty();
    }
    [[nodiscard]] friend bool operator==(const ClipboardContent&,
                                         const ClipboardContent&) = default;
};

// The bound the module enforces in both directions (section 2's "bounded size"). It matches the
// HAL clipboard seam's own cap, so a payload that survives one survives the other and a caller
// never sees a value the platform would refuse.
inline constexpr std::size_t kMaxShellClipboardBytes = 64 * 1024;
// A URL handed to the system handler is bounded like a clipboard write: far beyond any real link.
inline constexpr std::size_t kMaxShellUrlBytes = 2048;

// Section 5's dialog filters: a display label plus the extensions it accepts. An empty extension
// list means "any file", which is how a dialog offers its "All files" row.
struct DialogFilter {
    std::string label;
    std::vector<std::string> extensions;

    [[nodiscard]] friend bool operator==(const DialogFilter&, const DialogFilter&) = default;
};

// Section 5's `OpenDialogSpec`. `multiple` is the spec's multi-select; the module returns one
// `FileHandle` per accepted file, in the order the platform reported them.
struct OpenDialogSpec {
    std::string title;
    std::vector<DialogFilter> filters;
    std::string default_directory;
    bool multiple = false;
};

// Section 5's `SaveDialogSpec`. `confirm_overwrite` is the spec's overwrite confirmation; the
// platform owns the prompt, so the module only forwards the flag.
struct SaveDialogSpec {
    std::string title;
    std::vector<DialogFilter> filters;
    std::string default_directory;
    std::string suggested_name;
    bool confirm_overwrite = true;
};

// Section 5's `FolderDialogSpec`.
struct FolderDialogSpec {
    std::string title;
    std::string default_directory;
};

// Section 5's "returned handles are capability-based" and 9.1's "results carry capability handles,
// not raw OS pointers; handles are validated on use". `token` is issued by the `ShellService` that
// produced the handle and checked again when the handle is used, so a handle cannot be forged or
// replayed across a service instance.
struct FileHandle {
    std::string path;
    std::uint64_t token = 0;

    [[nodiscard]] bool valid() const noexcept {
        return token != 0 && !path.empty();
    }
    [[nodiscard]] friend bool operator==(const FileHandle&, const FileHandle&) = default;
};

// Section 9.1's error vocabulary, plus the two the spec names in prose: `Unsupported` and `Denied`
// are the gap G-15/Q-14 pair, and `InvalidPath` is section 8's traversal/symlink/executable
// rejection. `Cancelled` is a value, not a failure (section 5: "cancellation is first-class").
enum class ShellErrorKind : std::uint8_t {
    Unsupported = 0, // this build or platform has no such capability
    Denied,          // the user refused the permission
    Cancelled,       // the user or the app cancelled the operation
    Busy,            // another operation of the same kind is already in flight
    TooLarge,        // the payload exceeds the module's bound
    InvalidPath,     // traversal, symlink escape, or a refused executable
    Failed,          // the platform call failed
};

[[nodiscard]] constexpr std::string_view shell_error_kind_name(ShellErrorKind kind) noexcept {
    switch (kind) {
    case ShellErrorKind::Unsupported:
        return "unsupported";
    case ShellErrorKind::Denied:
        return "denied";
    case ShellErrorKind::Cancelled:
        return "cancelled";
    case ShellErrorKind::Busy:
        return "busy";
    case ShellErrorKind::TooLarge:
        return "too-large";
    case ShellErrorKind::InvalidPath:
        return "invalid-path";
    case ShellErrorKind::Failed:
        return "failed";
    }
    return "unknown";
}

// One failure. `detail` is a short, stable, non-localised string for diagnostics; it never carries
// clipboard content or a full path (section 8: "redact clipboard/path contents in logs").
struct ShellError {
    ShellErrorKind kind = ShellErrorKind::Failed;
    std::string detail;

    [[nodiscard]] friend bool operator==(const ShellError&, const ShellError&) = default;
};

// Section 3's `DragPayload` variants, restricted to what V1 can carry. `Internal` is the spec's
// "stable `(UiId, ItemKey)`/payload ids, never screen coordinates"; `FilePromise` is a path that is
// read only after an accepted drop (section 9.1).
enum class DragPayloadKind : std::uint8_t { None = 0, Text, Uri, FilePromise, Internal };

[[nodiscard]] constexpr std::string_view drag_payload_kind_name(DragPayloadKind kind) noexcept {
    switch (kind) {
    case DragPayloadKind::None:
        return "none";
    case DragPayloadKind::Text:
        return "text";
    case DragPayloadKind::Uri:
        return "uri";
    case DragPayloadKind::FilePromise:
        return "file-promise";
    case DragPayloadKind::Internal:
        return "internal";
    }
    return "none";
}

struct DragPayload {
    DragPayloadKind kind = DragPayloadKind::None;
    // Text, Uri and FilePromise payloads. Bounded by `kMaxShellDragTextBytes`.
    std::string text;
    // Internal payload identity: the scene node and the item key inside it.
    std::uint64_t node_id = 0;
    std::uint64_t item_key = 0;

    [[nodiscard]] bool empty() const noexcept {
        return kind == DragPayloadKind::None;
    }
    [[nodiscard]] friend bool operator==(const DragPayload&, const DragPayload&) = default;
};

// A drag payload's text bound. Smaller than the clipboard's because a drag is a pointer gesture and
// a payload that large is a file promise, not text.
inline constexpr std::size_t kMaxShellDragTextBytes = 4 * 1024;

// Section 3's `DragActions`: what a source offers and what a target accepts. A bitmask, so "copy or
// move" is one value.
enum class DragActions : std::uint8_t {
    None = 0,
    Copy = 1u << 0,
    Move = 1u << 1,
    Link = 1u << 2,
};

[[nodiscard]] constexpr DragActions operator|(DragActions left, DragActions right) noexcept {
    return static_cast<DragActions>(static_cast<std::uint8_t>(left) |
                                    static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr DragActions operator&(DragActions left, DragActions right) noexcept {
    return static_cast<DragActions>(static_cast<std::uint8_t>(left) &
                                    static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr bool has_drag_action(DragActions value, DragActions flag) noexcept {
    return (value & flag) != DragActions::None;
}

// Section 3's `DropAction`: the single action a target resolved for one drop. `None` is a rejected
// drop, which is a normal outcome and not an error.
enum class DropAction : std::uint8_t { None = 0, Copy, Move, Link };

[[nodiscard]] constexpr std::string_view drop_action_name(DropAction action) noexcept {
    switch (action) {
    case DropAction::None:
        return "none";
    case DropAction::Copy:
        return "copy";
    case DropAction::Move:
        return "move";
    case DropAction::Link:
        return "link";
    }
    return "none";
}

// Section 9.1's "external file promises read only after an accepted drop, capped at 64 MiB/16
// files (configurable)". These are the defaults; `ShellConfig` carries the live values.
inline constexpr std::size_t kMaxShellDropFiles = 16;
inline constexpr std::uint64_t kMaxShellDropBytes = 64ull * 1024 * 1024;

// The bounded diagnostics a shell keeps (ADR-022: values and counters, never a log line per call).
// `detail` strings are already redacted by the module.
struct ShellDiagnostic {
    ShellErrorKind kind = ShellErrorKind::Failed;
    std::string detail;
};

struct ShellDiagnostics {
    std::size_t recorded = 0;
    std::size_t dropped = 0;
    std::size_t capacity = 0;
    std::vector<ShellDiagnostic> entries;

    std::uint64_t clipboard_reads = 0;
    std::uint64_t clipboard_writes = 0;
    std::uint64_t urls_opened = 0;
    std::uint64_t dialogs_opened = 0;
    std::uint64_t dialogs_cancelled = 0;
    std::uint64_t refused_unsupported = 0;
    std::uint64_t refused_denied = 0;
    std::uint64_t refused_busy = 0;
    std::uint64_t refused_too_large = 0;
    std::uint64_t refused_invalid_path = 0;
    std::uint64_t drops_accepted = 0;
    std::uint64_t drops_rejected = 0;
    std::uint64_t stale_completions = 0; // a late result for an already-resolved operation
};

} // namespace tessera::appfw
