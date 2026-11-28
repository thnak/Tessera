# Typed text in the canvas (#601)

Status: engine side, Wasm export and page side implemented, including the caret-placed IME window and pointer
caret and drag selection (#980); the real-browser IME pass is a manual gate (see [§8](#8-what-is-left)). Related: [`canvas-input-session.md`](canvas-input-session.md) (the export table and
the keyboard rules this note extends), [`../specs/text-editing-v1.md`](../specs/text-editing-v1.md)
(the editing contract), [`input.md`](input.md).

## 1. The problem

A Studio canvas `TextInput` could be focused but never changed. Three things were missing, and none of them
is in the browser's gift:

1. The engine edits text (`EditController`, `CompositionState`, `TextInputRuntime`) but the preview session
   never attached one to a node, and `PreviewInput` had no kind that carries text.
2. The canvas export surface took keys (`tessera_canvas_key`) but nothing that takes text. A key is not a
   character: a soft keyboard, a dead key, an IME and a paste all produce text with no usable `keydown`.
3. A `<canvas>` receives no text events at all: no caret, no IME composition window, no soft keyboard.

## 2. Decisions

| Question | Decision | Why |
|---|---|---|
| Who edits | The engine's own `EditController` + `TextInputRuntime`, bound to the field node | One editing implementation, shared with native windows and generated components. The page forwards; it never edits. |
| How text reaches the engine | A new session input kind family (`Text`, `CompositionStart/Update/End`) dispatched through `PreviewSession::dispatch` and the production `InputRouter` | The router already delivers `EditText`/`Composition*` to the focused node; the preview adds no second path. |
| Hidden `textarea` or `EditContext` | A hidden `textarea` | `EditContext` is Chromium-only (no Firefox or Safari), and the studio targets all three. A textarea gets `beforeinput`, composition, the IME candidate window and mobile keyboards everywhere. The module boundary is the same either way (`tessera_canvas_text`), so an `EditContext` front end can replace the textarea later without touching the engine. |
| What the textarea stores | Nothing: it is emptied after every commit | It is a text *source*. The engine's buffer is the only copy, so there is no second value to drift. |
| Clipboard | The page's `copy`/`cut`/`paste` events on the textarea, engine owns the selection | The page cannot read the selection, so `copy` and `cut` ask the engine for it (`text` in the report) and the page writes the clipboard; `paste` is a commit. The engine never touches the OS clipboard in the preview. |
| Where the IME window opens | At the caret | The engine reports `caretBox` (logical pixels, the rectangle paint draws the caret at) and the page makes the textarea one caret-high line that starts there, scaled into layout pixels under the pane zoom. `fieldBox` is the fallback. |
| Pointer caret and drag selection | The engine, from the press and move it already receives | `tessera_canvas_pointer` places the caret on a press inside the focused field and extends the selection while the press is held. The page adds nothing: pointer capture already delivers the drag. |

## 3. Engine side

`SceneBuildResult::text_fields` lists every `TextInput`/`TextArea` the build walk produced (with its automation
id). The walk also paints the field's value (`value`, else `defaultValue`), which a field never did before.

`PreviewSession` keeps one `TextField` per id (`scene_session_text.cpp`): an `EditController` and a
`TextInputRuntime` attached to the node. The six editing events (`Text`, `EditText`, `CompositionStart/Update/End`,
`KeyDown`) are bound to the runtime and `TextChange` to the `onChange` body. The record outlives `rebuild()`
(matched by automation id), so the caret, selection, undo history and an IME composition survive the rebuild an
`onChange` triggers.

```
tessera_canvas_text(kind, text)                          wasm_canvas_entry.cpp
  → CanvasSceneSession::text(kind, text)                 scene_canvas_text.cpp
    → PreviewSession::dispatch(PreviewInput{Text|Composition*})
      → InputRouter → focused node → TextInputRuntime::handle_event
        → EditController edits the buffer; publish_decoration() puts the caret in the paint data
        → one TextChange per committed edit → on_text_change
          → onChange IR body with a TextChangeEvent record {value, target:{value}}
          → state changed → rebuild(); the controlled field is re-synced to the scope
```

- **Controlled fields.** A `value` prop makes the scope the owner: after the handler ran, the rebuild resets the
  buffer if the scope disagrees, so an `onChange` that does not store the text takes the edit back (as React
  does). A field with only `defaultValue` seeds once and keeps its buffer.
- **Caret and selection paint.** `TextInputRuntime::publish_decoration` writes them into the paint data, which the
  ordinary text paint already draws; `sync_text_fields()` runs after every dispatch and rebuild, so focus changes
  show or hide the caret. An unchanged decoration marks nothing dirty, so a static field still presents nothing.
- **Chunking.** An input event carries at most 256 payload bytes, so a longer commit (a paste) is split at code
  point boundaries into several events (several undo steps and `onChange` calls). A preedit is one payload and is cut.
- **Keys.** In a focused text field a printable key is *not* dispatched: its character arrives through `text()`.
  Dispatching it would double the character and make Space click the field. Editing keys (Backspace, Delete, the
  arrows, Home/End, Ctrl+A/Z/Y) go through `key()`, and `handled` is true only when the field's controller used
  the key, so a key it ignored stays the browser's.
- **Blur** cancels a composition in flight, as a browser does when its element loses focus.

### Caret geometry and the pointer (#980)

`paint::text_field_caret_box` and `text_field_hit` (`include/tessera/ui/paint/text_field_geometry.hpp`) resolve
the field the way `paint_text_lines` does (padding, the wrap decision, a single-line field's vertical centring,
line alignment) and then call the same `caret_box` paint calls, so the reported rectangle is the one drawn.
`CanvasSceneSession` asks for it after every report that has a focused field (`scene_canvas_session_caret.cpp`),
against the same cascade (`SceneStyleTree`) and the font the frame paints with (a provider the Wasm entry sets,
`canvas_paint_metrics()`; none means the stub, as in paint). While composing, the offset is where the composition
started, which is where the IME's window belongs.

A press inside the focused field maps its point to the nearest grapheme boundary (`text_field_hit`: the line by y,
then the point inside that line with the painted advances) and calls `place_caret_at`; the session remembers the
press (`selecting_text_`), and each move until the release calls `extend_selection_to` with the point clamped to the
text, so a drag that leaves the field still extends to the ends. A press outside the field starts nothing; a press
during a composition leaves the caret to the composition. Shift+click is not wired (the pointer call carries no
modifiers).

### The report

Every pointer, key and text report carries, while a text field holds focus: `textFocus`, `composing`,
`multiline`, `caret`, `selectionStart`, `selectionEnd` (UTF-8 byte offsets into the value), `fieldBox` and `caretBox`. With no
field focused none of them is present, so existing reports are byte-identical and a missing `textFocus` means "not
editing". `text` is present only on `copy`/`cut`.

## 4. Page side (`designer/web/src/views/canvas-text.ts`)

One `<textarea>` is added beside the canvas, hidden (`opacity:0`, `pointer-events:none`; not `display:none`
while it holds focus, because a `display:none` element cannot keep it) and not a tab stop. The page feeds it
every pointer and key report:

- `textFocus` true: show it over `fieldBox` and focus it one task later (a press focuses the canvas as its
  default action, after the pointer handler, and would undo an immediate focus).
- `textFocus` absent: hide it and hand the keyboard back to the canvas.

| Browser event on the textarea | Forwarded as |
|---|---|
| `beforeinput` `insertText`, `insertReplacementText`, `insertFromPaste/Drop` | `commit` with the text; default prevented |
| `beforeinput` `insertLineBreak/Paragraph` | `commit "\n"` (a single-line field drops it) |
| `beforeinput` `deleteContentBackward/Forward` | key `Backspace`/`Delete` down+up (soft keyboards that send no usable keydown) |
| `compositionstart/update` | `start` / `update` with the preedit |
| `compositionend` | `commit` with the result, or `end` (cancel) when it is empty; the textarea is emptied |
| `keydown`/`keyup` | `tessera_canvas_key`; prevented only when `handled`. Skipped while composing |
| `copy` / `cut` | `copy`/`cut`; the report's `text` goes to `clipboardData` |
| `paste` | `commit` with `clipboardData` text; default prevented |

### Keyboard rules kept

- **Never a keyboard trap.** Tab and Shift+Tab are the scene's rules. When the engine reports `left` the page
  neither prevents the default nor takes focus back, so the browser moves on. Shift+Tab out of the field would
  land on the canvas (the previous tab stop), which re-enters its tab order as a keyboard arrival, so the canvas
  is skipped for one task.
- **Ctrl/Cmd chords stay the browser's and the studio's**, except the field's own Select all, Undo and Redo
  (Ctrl/Cmd+A, Z, Shift+Z, Y). Cmd is mapped to the engine's Control, so one keymap serves every platform.
- **Focus bookkeeping.** The canvas does not send `blur` when the keyboard moves to its own textarea, and taking
  the focus back is not a keyboard arrival (no Tab is sent). Losing focus to anything else on the page drops the
  field and cancels a composition.

### Zoom

The textarea shares the canvas's parent, so the pane's transform scales both. It is positioned in layout pixels:
the canvas's offset inside the host divided by `cssZoomOf(rect.width, offsetWidth)` (#600), plus `fieldBox` times
the canvas's CSS pixels per logical pixel.

## 5. Wasm

`_tessera_canvas_text` (and `_tessera_canvas_key`, which was kept alive only by `EMSCRIPTEN_KEEPALIVE`) are in
the canvas module's `EXPORTED_FUNCTIONS` list in `src/compiler/codegen/CMakeLists.txt`. The canvas module links
`tessera_text_edit`, as every target that compiles the preview sources now does.

## 6. Tests

- `tests/unit/scene_canvas_text_tests.cpp` (ctest `scene_canvas_text`): typing reaches the `onChange` handler
  and the state, Backspace/Delete/arrows/Home and caret-position insertion, printable keys left to the text path,
  IME start/update/commit/cancel and blur mid-composition, select-all, copy, cut, paste and undo, a 700-byte paste,
  a controlled field taking back an edit its handler did not store, no field focused, the report fields.
- `scripts/ci/preview_canvas_text.mjs` (CI: Pages workflow): the textarea against a fake DOM: placement under a
  pane zoom, the caret-high placement, `beforeinput`, composition, clipboard, chord filtering, Tab `left`, Shift+Tab,
  focus hand-off.
- `tests/unit/scene_canvas_text_caret_tests.cpp` (ctest `scene_canvas_text_caret`): the caret rectangle moves right with
  the offset and stays inside the field, the composition offset, `caretBox` in the report, a press placing the caret
  (nearest boundary, both ends, typing lands there), a drag selecting, extending past the field and collapsing back,
  and a press outside the field or during a composition leaving the caret alone.

## 7. Alternatives rejected

- **Forward `keydown` and synthesize characters from `key`.** Wrong for IMEs, dead keys, soft keyboards and
  paste; the engine would need a keyboard layout.
- **`EditContext` first.** Cleaner geometry control, but not available in Firefox or Safari.
- **A visible, styled input.** A second rendering of the field that could disagree with the canvas.

## 8. What is left

- **A real browser pass with an IME is a manual gate**, not automated: no CI lane drives a real IME. Before a release that
  touches this path, on Chrome, Firefox and Safari (and one phone), with a Pinyin or Japanese IME: focus a TextInput by
  click; type a composition and check the candidate window opens at the caret, not at the field's corner; move the
  caret by click and by arrow keys mid-text and compose again (the window follows); commit, then cancel one with
  Escape; drag-select across the field and copy. Record browser versions in the PR.
- **Mobile Backspace in an empty field**: some soft keyboards send nothing when the textarea is empty. A
  sentinel character would fix it and is not done.
- Components that instantiate a `TextInput` inside an instance frame are built but their `onChange` runs
  against the instance's scope; the root-component path is the tested one.
- **Shift+click and double-click word selection** in the field (the pointer call carries no modifiers or click count).
