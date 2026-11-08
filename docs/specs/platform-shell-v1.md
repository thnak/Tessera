# Platform shell v1: clipboard, drag/drop, menus, file dialogs

Status: proposed (Big Step F, defined in [AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step), section "Big Steps").
Decision: [ADR-030](../../decisions/ADR-030-app-platform-completeness.md) (app-platform
completeness: navigation, restore, shell, i18n, accessibility, data). Companion to
[`services.md`](../design/services.md), [`input.md`](../design/input.md),
[`security.md`](../design/security.md), and [`mobile-integration.md`](../design/mobile-integration.md).

The platform shell is the OS-facing app surface outside the render tree. It is async,
permission-aware, and never blocks the UI thread.

## 1. Capabilities

| Capability | Windows | macOS | Linux | Android | iOS |
|---|---|---|---|---|---|
| Clipboard text | yes | yes | yes | yes | yes |
| Clipboard files/URI | later | later | later | later | later |
| Drag/drop internal | yes | yes | yes | limited | limited |
| Drag/drop external files | yes | yes | yes | yes | yes |
| Context menu | yes | yes | yes | long-press | long-press |
| File open/save dialog | yes | yes | portal/dialog | SAF | UIDocumentPicker |
| Menu bar | yes | yes | app menu | no | no |
| Tray/status item | yes | yes | later | no | no |
| Global shortcuts | later | later | later | no | no |

In the table, "later" means not in V1 (V1 = first-release scope, [AGENTS.md](../../AGENTS.md#non-negotiables) rule 8);
no later release is assigned in this spec. SAF = Android Storage Access Framework;
`UIDocumentPicker` = the iOS document picker; "portal/dialog" = the XDG desktop portal or a
toolkit dialog on Linux. Unsupported capabilities degrade explicitly; no silent no-op.

## 2. Clipboard

```cpp
Task<ClipboardContent> read_clipboard();
Task<void> write_clipboard(ClipboardContent);
bool has_clipboard_format(ClipboardFormat);
```

- V1: `text/plain`; after V1 (no release assigned): HTML/RTF/image/file list.
- Read/write async, bounded size, permission-aware; never block the UI thread.
- Copy/cut/paste commands in text editing use this service.
- Secure fields disable clipboard by default; never log content.
- Preserve line endings/trailing newline policy; normalize on paste.
- Clipboard content is untrusted: no HTML/JS execution, no path traversal.

## 3. Drag and drop

```cpp
struct DragSource {
    DragPayload payload;       // text/uri/file promise
    DragImage image;
    DragActions actions;       // copy/move/link
};

struct DropTarget {
    bool can_accept(const DragPayload&);
    DropAction on_drop(const DropPayload&, Point local);
};
```

- Internal drag: stable ids/payload, no serialization unless crossing processes.
- External drag: file promises/URIs; validate paths, types, sizes; no arbitrary file reads
  until drop is accepted.
- Drag image/preview is a snapshot; autoscroll at edges; keyboard alternative required.
- Drop target decides copy/move/link; destructive move asks/confirms.
- Cancel/escape cancels; no callbacks after window/context teardown.
- Touch: long-press drag with platform-native behavior (the platforms that provide it are not
  enumerated here; the table in section 1 marks Android/iOS internal drag/drop as limited).

## 4. Context menus

- Declarative menu model (items, submenu, shortcut, enabled/checked, separators).
- Open at pointer/keyboard focus; flip/shift/clamp to window/safe area.
- Focus/dismiss/escape/back follow overlay rules; menu is a modal-ish overlay.
- Destructive items require confirmation; disabled reasons are accessible.
- Menus are app-owned; no arbitrary native command execution.

## 5. File dialogs

```cpp
Task<std::optional<FileHandle>> open_file(OpenDialogSpec);
Task<std::optional<Path>> save_file(SaveDialogSpec);
Task<std::optional<Path>> pick_folder(FolderDialogSpec);
```

- Filters, multi-select, default directory, overwrite confirmation.
- Paths are validated/sandboxed; no traversal; returned handles are capability-based.
- Async; cancellation is first-class; no blocking modal loop on the UI thread.
- Recent documents/favorites are platform services, not framework globals.
- Mobile document pickers map to the same API; security-scoped URLs handled by the HAL (hardware/OS abstraction layer,
  [`hal.md`](../design/hal.md)).

## 6. Menu bar, tray, shortcuts

- App menu model: app/file/edit/view/window/help; platform conventions win.
- Tray/status items: icon, tooltip, menu, click action; battery/visibility policy (the policy
  is not specified here).
- Global/menu shortcuts are registered declaratively; conflicts are diagnostics.
- Shortcuts are disabled while IME (input method editor) composition is active unless
  the shortcut is explicitly marked to allow it (the marking/API is not specified).
- No command execution from untrusted labels/paths.

## 7. Threading and errors

- All shell operations are async tasks on `io`/platform executors (see
  [`async.md`](../design/async.md)); results on UI thread.
- Errors are values (`Result`); permission denial is a normal result, not a crash.
- Bound concurrency/queue; cancel on window/app teardown.
- No OS handles leak into UI/components; use capability handles.

## 8. Security

- Treat clipboard/drop/dialog payloads as untrusted.
- Validate file paths, URIs, sizes, MIME types; reject symlink/traversal attacks.
- Do not auto-open dropped executables; confirm risky actions.
- Redact clipboard/path contents in logs/snapshots.
- Permissions are requested just-in-time with rationale; denial degrades gracefully.

## 9. Testing

- Clipboard round-trip, formats, permissions, size limits, secure fields.
- Internal/external DnD, paths, cancel, autoscroll, drop actions.
- Context menu positioning/focus/dismiss/shortcuts.
- File dialogs: open/save/folder, filters, cancel, overwrite, sandbox.
- Menu/tray/shortcut registration/conflicts.
- Headless shell fakes; deterministic async; no real OS dialogs in CI.

## 9.1 Detail design

Capability negotiation:

```cpp
enum class ShellCapability {
    ClipboardText, ClipboardFiles, DragInternal, DragExternal, DropFiles,
    ContextMenu, FileOpen, FileSave, FolderPick, MenuBar, Tray, GlobalShortcut
};

struct ShellCapabilities {
    std::uint64_t supported;   // bitmask
    std::uint64_t permissioned;
};
```

- Query capability/permission at startup and on permission changes; never branch on platform
  `#ifdef` in UI code.
- Unsupported capability returns `Error::Unsupported`; the component hides/disables the action
  with an accessible reason.
- Permission denial is a normal result, not a crash; do not repeatedly prompt.

`ShellService` contract:

```cpp
class ShellService {
public:
    Task<Result<ClipboardContent>> read_clipboard();
    Task<Status> write_clipboard(ClipboardContent);
    Task<Result<std::optional<FileHandle>>> open_file(OpenDialogSpec);
    Task<Result<std::optional<Path>>> save_file(SaveDialogSpec);
    Task<Result<std::optional<Path>>> pick_folder(FolderDialogSpec);
};
```

- All operations return cancellable tasks; completion is posted to the UI thread.
- Results carry capability handles, not raw OS pointers; handles are validated on use.
- One operation at a time per dialog/menu; whether a second request is queued or rejected is
  not specified here.

Drag/drop state machine:

```text
Idle -> DragEnter -> DragOver <-> DragLeave
                    \-> Drop -> Accepted/Rejected
Any -> Cancelled (escape/window blur/target removed)
```

- `DragPayload` variants: text, URI, file promise, internal node/key. File promises are read only
  after an accepted drop and within a size/time budget.
- Drag source declares copy/move/link; target accepts a subset; move requires an explicit opt-in (its form is not specified).
- Autoscroll is driven by the scroll system; no blocking loop.
- Internal DnD uses stable `(UiId, ItemKey)`/payload ids, never screen coordinates.

Menus/shortcuts:

- Menu model is declarative (id, label key, shortcut, enabled/checked, children).
- Shortcut conflicts are detected at build/startup; platform-reserved shortcuts are not stolen.
- Menus/shortcuts are disabled during IME composition unless the shortcut is explicitly marked to allow it (marking not specified).
- Context menu opening follows overlay positioning/focus/dismiss rules.

Failure/edge cases:

- Window/app teardown cancels pending dialogs/drag; no callbacks after teardown.
- Path traversal/symlink/executable drop -> reject with diagnostic.
- Clipboard oversized/unsupported format -> error/ignore, never partial decode.
- Mobile SAF/security-scoped URLs are held only for the operation lifetime.
- Tray/menu bar unavailable -> hide action; no silent no-op.

Gap-closure decisions:

Gap/question IDs (`G-15`, `Q-14`) refer to
[`app-completeness-gap-resolutions-v1.md`](../architecture/app-completeness-gap-resolutions-v1.md).

- **G-15/Q-14**: capabilities are a bitmask; unsupported/permission-denied returns
  `Unsupported/Denied`, never a silent no-op. External file promises read only after an accepted
  drop, capped at 64 MiB/16 files (configurable). Component shortcuts are scoped to the focused
  subtree; app/global shortcuts win; conflicts are startup diagnostics. Cross-process DnD and
  Linux tray are best-effort and not in V1.

## 10. Deferred/open questions

Items are open (no decision yet).

- Clipboard file/URI formats and per-platform permission prompts.
- Global shortcuts vs accessibility conflicts.
- Linux tray/status-item matrix.
- Security-scoped URL lifetime on iOS/macOS.
- Cross-process drag promise implementation.
