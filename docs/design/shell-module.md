# The shell module: clipboard, dialogs, drag and drop

Status: implemented (issue #450). The normative contract is
[`platform-shell-v1.md`](../specs/platform-shell-v1.md); this note explains the shape the code took,
what it deliberately leaves out, and why each boundary sits where it does. The decision behind the
module is [ADR-030](../../decisions/ADR-030-app-platform-completeness.md).

The module is `tessera_appfw_shell` (`src/appfw/shell*.cpp`) plus the Win32 back end
`tessera_appfw_shell_win32` (`src/platform/windows/shell_win32*.cpp`). It is a layer-1 module in the
[ADR-050](../../decisions/ADR-050/README.md) sense: it depends on `appfw_core` and layer 0, and it
must not reach into the UI layer.

## 1. The split: policy in the module, platform in the back end

The single most important decision in this module is that **validation and policy are not the back
end's job**. `shell_backend.hpp` is the whole platform boundary, and it names no `HWND`, no
`IDropTarget` and no `CLSID`. Everything that must be true on every platform lives in
`ShellService`:

| Rule | Where | Why there |
|---|---|---|
| the clipboard size bound | module | one bound, one spelling; a back end cannot be trusted to agree |
| a secure field's clipboard is refused | module | it is a policy about the *field*, not about the OS |
| one operation at a time per kind (`Busy`) | module | it is a policy about the app, not about the OS |
| path validation (traversal, executable) | module | one rule for every back end, and it is the security boundary |
| capability handles (`FileHandle::token`) | module | a back end must not be able to forge one |
| the drop file/byte budget | module | section 9.1's cap is the app's, not the platform's |
| the actual clipboard call | back end | only the platform can do it |
| the dialog's modal loop | back end | only the platform has one |
| what the platform can do | back end reports, module caches | the platform is the only thing that knows |

The payoff is the contract suite: `appfw_shell_tests` links `tessera_appfw_shell` and nothing
platform-specific, so the same tests run on Windows, Linux, lavapipe and the Wasm lane. A back end is
then a small class whose only job is to turn a request into a result.

## 2. Capability negotiation

Section 9.1's two masks are kept separate because "this build cannot" and "the user refused" are
different answers, and a component renders them differently: one hides the action, the other disables
it with an accessible reason. `ShellCapabilities::usable` is the one predicate a caller branches on.

`ShellService` caches the set at construction and re-queries it in `refresh_capabilities()`, which a
permission change calls. That is section 9.1's "query at startup and on permission changes" taken
literally: a per-operation query would be a platform call on a hot path, and a never-refreshed cache
would make a granted permission invisible until restart.

The consequence for tests is worth stating because it cost a debugging round: a test that narrows the
capability set must do it **before** the service is constructed, which is why the test harness takes
the set as a constructor argument rather than exposing a setter.

## 3. Path validation is the security boundary

Section 8 is the reason `shell_path.cpp` exists as its own file with its own test TU. A path is
accepted when it is non-empty, within `kMaxShellPathBytes`, free of NUL, free of a `..` component,
absolute, and not an executable extension.

Two of those checks are the ones a naive implementation gets wrong:

- a `..` **inside a file name** (`a..b.txt`) is not a component and is accepted;
- a **drive-relative** Windows path (`C:foo`) is not absolute, and must not slip through a check that
  only looks for a colon.

**What it deliberately does not do: touch the file system.** No `stat`, no `realpath`, no symlink
resolution. That is a decision, not an omission. A symlink check needs a syscall, a syscall is a
block, and this function runs on the UI thread while a drop is being resolved. The back end - which
already runs on an `IoTaskExecutor` worker - is where a resolved path is opened, and it is the
platform's own open that refuses a symlink escape. What this function guarantees is that the *string*
the app is handed cannot be a traversal or an executable, which is the part that can be decided
without I/O.

The refused-extension list is short and explicit rather than a "known bad" list, because a deny-list
of every executable form is always incomplete. The test asserts the whole list rather than a sample,
so a list that lost an entry fails.

## 4. Capability handles

Section 5 says a dialog returns capability handles, not raw OS pointers, and section 9.1 says handles
are validated on use. `FileHandle` is a path plus a token the service issued; `validate()` checks the
**pair** against the service's own table. That makes three attacks fail:

- a forged token (not in the table);
- a real token with a different path (the pair does not match);
- a handle from another service instance (a different table).

`revoke()` removes the entry, so a handle can be retired when the app is done with it.

## 5. Async shape and cancellation

Section 7 says every operation is an async task on an `io`/platform executor with its result on the UI
thread. The seam's `start_*` methods already have that shape: they return at once and post their
completion through the injected `CompletionQueue`. On top of that the service offers two forms:

- `submit_*`, the coroutine-free form, for a caller that is not itself a coroutine (a frame tick, an
  event handler);
- `read_clipboard`/`open_file`/..., the `runtime::Task` form, for a caller that is.

The task's own token is deliberately empty, exactly as `NetClient::send` documents: cancelling it
would make the runtime throw `TaskCancelled` at the `co_await`, and section 5 wants `cancelled` **as a
value**. Cancellation therefore flows through the operation's own token, and a cancelled dialog is an
empty result, not an exception.

The awaiter is the same shape `NetClient::send` uses, for the same reason: the completion can arrive
before the coroutine reaches `await_suspend`, so "which came first" is carried in shared state rather
than assumed, and a destroyed frame is never resumed.

## 6. The Win32 back end

- **Clipboard** goes through `hal::Clipboard` (`tessera_hal_clipboard`), which already owns the
  `OpenClipboard` retry budget and the UTF-16 conversion. A second implementation here would be a
  second answer to "what happens when another process holds the clipboard".
- **Dialogs** use `IFileOpenDialog`/`IFileSaveDialog` with `FOS_PICKFOLDERS`. The modal loop is the
  interesting constraint: `Show` pumps messages until the user answers, so it runs on an
  `IoTaskExecutor` worker, never on the UI thread. `CoInitializeEx` is per-thread, so the worker
  initializes COM itself; `RPC_E_CHANGED_MODE` is handled as "already initialized", not as an error,
  because a pooled worker may have been initialized by an earlier task.
- **External file drop** is the `WM_DROPFILES` path, which needs no OLE registration and no
  `IDropTarget` on the window. `win32_read_dropped_files` copies the paths and bounds them at
  `kMaxShellDropFiles`; it never opens a file. Section 3's *internal* drag is the scene's own gesture
  pipeline (a `DragRecognizer` claim), not a platform drag, so the platform only has to answer "a file
  was dropped here".

## 7. What is not here

Named so the next slice does not have to rediscover it:

- **Clipboard files and URIs** - "later" in the spec's section 1 table, and not representable in
  `ClipboardContent` on purpose, so a caller cannot ask for a format the module would have to fake.
- **Cross-process drag promises** - section 10 lists it as an open question.
- **The menu bar and tray models** - `window-chrome-v1.md` owns the menu *model*; this module owns
  only the capability bits.
- **Global shortcuts** - "later" in the spec's table.
- **OLE drag-and-drop with a drag image** - the `WM_DROPFILES` path is the V1 mechanism; an
  `IDropTarget` would add OLE registration to every window for a drag image the engine can draw
  itself.
- **Every non-Windows back end** - Linux/X11, macOS, Android and iOS. The seam is ready for them; the
  contract suite is what they will be measured against.
- **The `useClipboard` hook and the text-editing commands** - `use-clipboard-v1.md` and
  `text-editing-v1.md` section 12 own those; they consume this service.

## 8. Tests

`appfw_shell_tests` (`tests/unit/appfw_shell_*`), one TU per acceptance group so a failure names the
rule:

| TU | Covers |
|---|---|
| `appfw_shell_path_tests.cpp` | the section 8 rule: absolute forms, traversal, executables, check order |
| `appfw_shell_capability_tests.cpp` | section 9.1: `Unsupported` vs `Denied`, `require`, refresh |
| `appfw_shell_clipboard_tests.cpp` | section 2: round trip, bound, secure field, one at a time, cancel |
| `appfw_shell_dialog_tests.cpp` | section 5: open/save/folder, cancel as a value, handles, path refusal |
| `appfw_shell_drop_tests.cpp` | section 3: payload kinds, budget, drop action as a value |
| `appfw_shell_task_tests.cpp` | the coroutine forms and their awaiter |

Determinism is structural: `ManualClock`, `FakeShell` and a hand-drained `CompletionQueue`. No test
opens a dialog, touches a real clipboard, reads a wall clock, sleeps, starts a thread or touches the
file system.

Related: [`platform-shell-v1.md`](../specs/platform-shell-v1.md),
[`services/README.md`](services/README.md), [`networking-module.md`](networking-module.md) (the same
seam shape for the network), [`storage-module.md`](storage-module.md),
[`security.md`](security.md), [ADR-030](../../decisions/ADR-030-app-platform-completeness.md),
[ADR-050](../../decisions/ADR-050/README.md).
