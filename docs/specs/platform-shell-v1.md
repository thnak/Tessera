# Platform shell v1: clipboard, drag/drop, menus, file dialogs

Status: proposed (Big Step F). Decision: ADR-030. Companion to
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

Unsupported capabilities degrade explicitly; no silent no-op.

## 2. Clipboard

```cpp
Task<ClipboardContent> read_clipboard();
Task<void> write_clipboard(ClipboardContent);
bool has_clipboard_format(ClipboardFormat);
```

- V1: `text/plain`; future: HTML/RTF/image/file list.
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
- Touch: long-press drag with platform-native behavior where available.

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
- Mobile document pickers map to the same API; security-scoped URLs handled by HAL.

## 6. Menu bar, tray, shortcuts

- App menu model: app/file/edit/view/window/help; platform conventions win.
- Tray/status items: icon, tooltip, menu, click action; battery/visibility policy.
- Global/menu shortcuts are registered declaratively; conflicts are diagnostics.
- Shortcuts are disabled while IME composition is active unless explicitly allowed.
- No command execution from untrusted labels/paths.

## 7. Threading and errors

- All shell operations are async tasks on `io`/platform executors; results on UI thread.
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

## 10. Open questions

- Clipboard file/URI format and permissions on each platform.
- Drag promise lifetime and cross-process DnD.
- Tray support matrix on Linux.
- Global shortcuts/accessibility conflicts.
- Document picker security-scoped URLs on iOS/macOS.
